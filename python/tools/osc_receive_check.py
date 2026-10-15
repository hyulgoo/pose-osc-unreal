"""OSC receive checker for ``/dstrict/hands`` (DESIGN_DECISIONS.md D3).

Deliberately independent of the sender: standard library only, no python-osc, no import from pose_osc.
The datagram is parsed byte by byte against the D3 spec literals below, so a bug shared by the sender's
encoder cannot hide here. The seq rules are the same ones the Unreal receiver implements (D3, D11):

  - Idle (no *accepted* packet for ``idle_timeout``, or nothing received yet): the next packet resets the
    seq baseline.
  - seq > last: accept; the gap minus one is counted as lost.
  - seq == last: duplicate, drop.
  - last - seq > regress_threshold: sender restart (or int32 wrap), reset the baseline and accept.
  - otherwise (small regression): stale / reordered, drop.
  Only accepted packets refresh the idle timer.

Usage:
  python tools/osc_receive_check.py [--host 127.0.0.1] [--port 8000] [--duration N] [--json]
"""

from __future__ import annotations

import argparse
import json
import math
import socket
import struct
import sys
import time
from collections import Counter
from dataclasses import asdict, dataclass, field

# ----- D3 spec literals (do not import these from the sender) ----------------------------------------------
EXPECTED_ADDRESS = "/dstrict/hands"
EXPECTED_TYPE_TAGS = ",iiffffffff"
EXPECTED_SIZE = 68
DEFAULT_IDLE_TIMEOUT_S = 1.0  # D6: Idle after 1.0 s without packets
DEFAULT_REGRESS_THRESHOLD = 30  # D11: ~1 s of frames at 30 Hz


class SchemaError(ValueError):
    def __init__(self, kind: str, detail: str) -> None:
        super().__init__(f"{kind}: {detail}")
        self.kind = kind


def _read_osc_string(data: bytes, offset: int, what: str) -> tuple[str, int]:
    end = data.find(b"\x00", offset)
    if end < 0:
        raise SchemaError("truncated", f"{what} has no NUL terminator")
    raw = data[offset:end]
    padded_end = (end + 4) & ~3  # NUL included, then pad to a multiple of 4
    if padded_end > len(data):
        raise SchemaError("truncated", f"{what} padding runs past the packet end")
    if any(data[end:padded_end]):
        raise SchemaError("padding", f"{what} padding bytes are not all NUL")
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as e:
        raise SchemaError("encoding", f"{what} is not ASCII") from e
    return text, padded_end


@dataclass(frozen=True, slots=True)
class HandValue:
    x: float
    y: float
    z: float
    conf: float


@dataclass(frozen=True, slots=True)
class HandsPacket:
    seq: int
    person: int
    left: HandValue
    right: HandValue


def parse_hands(data: bytes) -> HandsPacket:
    """Strict structural parse. Raises SchemaError when the packet does not match D3 exactly."""
    if data.startswith(b"#bundle"):
        raise SchemaError("bundle", "OSC bundle received; D3 uses one plain message per frame")
    address, off = _read_osc_string(data, 0, "address")
    if address != EXPECTED_ADDRESS:
        raise SchemaError("address", f"{address!r} != {EXPECTED_ADDRESS!r}")
    tags, off = _read_osc_string(data, off, "type tags")
    if tags != EXPECTED_TYPE_TAGS:
        raise SchemaError("typetag", f"{tags!r} != {EXPECTED_TYPE_TAGS!r}")
    if len(data) != EXPECTED_SIZE:
        raise SchemaError("size", f"{len(data)} bytes != {EXPECTED_SIZE}")
    seq, person, lx, ly, lz, lc, rx, ry, rz, rc = struct.unpack_from(">ii8f", data, off)
    return HandsPacket(seq, person, HandValue(lx, ly, lz, lc), HandValue(rx, ry, rz, rc))


def value_warnings(p: HandsPacket) -> list[str]:
    """Range rules from D3. These do not reject the packet; they are counted and reported."""
    out: list[str] = []
    if p.seq < 0:
        out.append("seq_negative")
    if p.person not in (0, 1):
        out.append("person_not_0_1")
    for name, h in (("left", p.left), ("right", p.right)):
        if not all(math.isfinite(v) for v in (h.x, h.y, h.z, h.conf)):
            out.append(f"{name}_non_finite")
            continue
        if not (0.0 <= h.conf <= 1.0):
            out.append(f"{name}_conf_range")
        if not (0.0 <= h.x <= 1.0 and 0.0 <= h.y <= 1.0):
            out.append(f"{name}_xy_range")
        if p.person == 0 and (h.x, h.y, h.z, h.conf) != (0.0, 0.0, 0.0, 0.0):
            out.append(f"{name}_heartbeat_not_zero")
    return out


# ----- seq rules ---------------------------------------------------------------------------------------------
@dataclass(frozen=True, slots=True)
class SeqDecision:
    accepted: bool
    kind: str  # first | idle_reset | regress_reset | next | gap | duplicate | stale
    lost: int = 0


class SeqTracker:
    def __init__(self, idle_timeout_s: float = DEFAULT_IDLE_TIMEOUT_S, regress_threshold: int = DEFAULT_REGRESS_THRESHOLD) -> None:
        self.idle_timeout_s = idle_timeout_s
        self.regress_threshold = regress_threshold
        self._last_seq: int | None = None
        self._last_accept_t: float | None = None

    def is_idle(self, now: float) -> bool:
        return self._last_accept_t is None or now - self._last_accept_t > self.idle_timeout_s

    def on_packet(self, seq: int, now: float) -> SeqDecision:
        if self._last_seq is None or self.is_idle(now):
            kind = "first" if self._last_seq is None else "idle_reset"
            return self._accept(seq, now, kind, 0)
        last = self._last_seq
        if seq > last:
            lost = seq - last - 1
            return self._accept(seq, now, "gap" if lost else "next", lost)
        if seq == last:
            return SeqDecision(False, "duplicate")
        if last - seq > self.regress_threshold:
            return self._accept(seq, now, "regress_reset", 0)
        return SeqDecision(False, "stale")

    def _accept(self, seq: int, now: float, kind: str, lost: int) -> SeqDecision:
        self._last_seq = seq
        self._last_accept_t = now
        return SeqDecision(True, kind, lost)


# ----- reporting ---------------------------------------------------------------------------------------------
class _Jitter:
    """Mean |p[t] - 2 p[t-1] + p[t-2]| (x, y) over consecutive accepted packets with conf >= min_conf."""

    def __init__(self, min_conf: float = 0.6) -> None:
        self.min_conf = min_conf
        self._p1: tuple[float, float] | None = None
        self._p2: tuple[float, float] | None = None
        self.total = 0.0
        self.count = 0

    def add(self, h: HandValue, contiguous: bool) -> None:
        if h.conf < self.min_conf or not contiguous:
            self._p1 = self._p2 = None
            if h.conf < self.min_conf:
                return
        p = (h.x, h.y)
        if self._p1 is not None and self._p2 is not None:
            self.total += math.hypot(p[0] - 2 * self._p1[0] + self._p2[0], p[1] - 2 * self._p1[1] + self._p2[1])
            self.count += 1
        self._p2, self._p1 = self._p1, p

    @property
    def mean(self) -> float:
        return self.total / self.count if self.count else math.nan


@dataclass(slots=True)
class Summary:
    duration_s: float = 0.0
    received: int = 0
    accepted: int = 0
    lost: int = 0
    duplicate: int = 0
    stale: int = 0
    resets: dict[str, int] = field(default_factory=dict)  # first / idle_reset / regress_reset
    malformed: dict[str, int] = field(default_factory=dict)
    value_warnings: dict[str, int] = field(default_factory=dict)
    idle_events: int = 0
    rate_hz: float = 0.0  # 1000 / mean interval between consecutive accepted packets (same epoch)
    loss_rate: float = 0.0  # lost / (accepted + lost)
    interval_ms_mean: float = math.nan
    interval_ms_max: float = math.nan
    person_ratio: float = 0.0
    jitter_left: float = math.nan
    jitter_right: float = math.nan
    last_seq: int | None = None


class Checker:
    def __init__(self, idle_timeout_s: float, regress_threshold: int, verbose: bool = False) -> None:
        self.tracker = SeqTracker(idle_timeout_s, regress_threshold)
        self.verbose = verbose
        self.s = Summary()
        self._resets: Counter[str] = Counter()
        self._malformed: Counter[str] = Counter()
        self._warnings: Counter[str] = Counter()
        self._first_accept_t: float | None = None
        self._last_accept_t: float | None = None
        self._intervals: list[float] = []
        self._person = 0
        self._jl = _Jitter()
        self._jr = _Jitter()
        self._was_idle = True
        self.last: HandsPacket | None = None
        # window counters for the periodic line
        self.w_accepted = 0
        self.w_lost = 0
        self.w_intervals: list[float] = []

    def on_datagram(self, data: bytes, now: float) -> None:
        self.s.received += 1
        try:
            p = parse_hands(data)
        except SchemaError as e:
            self._malformed[e.kind] += 1
            if self.verbose:
                print(f"  형식 오류 {e}")
            return
        for w in value_warnings(p):
            self._warnings[w] += 1

        d = self.tracker.on_packet(p.seq, now)
        if not d.accepted:
            if d.kind == "duplicate":
                self.s.duplicate += 1
            else:
                self.s.stale += 1
            if self.verbose:
                print(f"  버림 seq={p.seq} ({d.kind})")
            return

        if d.kind in ("first", "idle_reset", "regress_reset"):
            self._resets[d.kind] += 1
            if d.kind != "first":
                print(f"  seq 기준 초기화 ({d.kind}) seq={p.seq}")
        contiguous = d.kind == "next"
        if self._last_accept_t is not None and d.kind in ("next", "gap"):
            iv = (now - self._last_accept_t) * 1000.0
            self._intervals.append(iv)
            self.w_intervals.append(iv)
        if self._first_accept_t is None:
            self._first_accept_t = now
        self._last_accept_t = now
        self.s.accepted += 1
        self.s.lost += d.lost
        self.w_accepted += 1
        self.w_lost += d.lost
        self._person += int(p.person == 1)
        self._jl.add(p.left, contiguous)
        self._jr.add(p.right, contiguous)
        self.last = p
        self._was_idle = False
        if self.verbose:
            print(f"  seq={p.seq} person={p.person} L={_fmt_hand(p.left)} R={_fmt_hand(p.right)} ({d.kind})")

    def on_tick(self, now: float) -> None:
        if not self._was_idle and self.tracker.is_idle(now):
            self._was_idle = True
            self.s.idle_events += 1
            print(f"  IDLE: {self.tracker.idle_timeout_s:.1f}초 동안 채택된 패킷 없음 (다음 패킷에서 seq 기준 초기화)")

    def window_line(self, elapsed_total: float, window_s: float) -> str:
        hz = self.w_accepted / window_s if window_s > 0 else 0.0
        total = self.w_accepted + self.w_lost
        loss = 100.0 * self.w_lost / total if total else 0.0
        if self.w_intervals:
            iv = f"간격 avg {sum(self.w_intervals) / len(self.w_intervals):.1f} / max {max(self.w_intervals):.1f}ms"
        else:
            iv = "간격 -"
        if self.last is not None:
            p = self.last
            latest = f"seq {p.seq} person {p.person} | L {_fmt_hand(p.left)} | R {_fmt_hand(p.right)}"
        else:
            latest = "아직 수신 없음"
        cum = (
            f"누적 형식오류 {sum(self._malformed.values())} 값경고 {sum(self._warnings.values())}"
            f" 중복 {self.s.duplicate} 역순 {self.s.stale} 재시작 {self._resets['idle_reset'] + self._resets['regress_reset']}"
        )
        line = f"[{elapsed_total:6.1f}s] {hz:5.1f}Hz ({iv}) | 유실 {loss:4.1f}% ({self.w_lost}/{total}) | {latest} | {cum}"
        self.w_accepted = self.w_lost = 0
        self.w_intervals = []
        return line

    def summary(self, duration_s: float) -> Summary:
        s = self.s
        s.duration_s = duration_s
        s.resets = dict(self._resets)
        s.malformed = dict(self._malformed)
        s.value_warnings = dict(self._warnings)
        total = s.accepted + s.lost
        s.loss_rate = s.lost / total if total else 0.0
        if self._intervals:
            # Rate from accepted-to-accepted intervals within an epoch, so Idle gaps (freeze, restart) don't
            # dilute it.
            s.interval_ms_mean = sum(self._intervals) / len(self._intervals)
            s.interval_ms_max = max(self._intervals)
            s.rate_hz = 1000.0 / s.interval_ms_mean if s.interval_ms_mean > 0 else 0.0
        s.person_ratio = self._person / s.accepted if s.accepted else 0.0
        s.jitter_left = self._jl.mean
        s.jitter_right = self._jr.mean
        s.last_seq = self.last.seq if self.last else None
        return s


def _fmt_hand(h: HandValue) -> str:
    return f"({h.x:.3f}, {h.y:.3f}, {h.z:+.3f}) c{h.conf:.2f}"


def _json_safe(v: object) -> object:
    if isinstance(v, float) and not math.isfinite(v):
        return None
    if isinstance(v, dict):
        return {k: _json_safe(x) for k, x in v.items()}
    return v


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="/dstrict/hands 수신 검사기 (D3 스키마·seq 규칙)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--idle-timeout", type=float, default=DEFAULT_IDLE_TIMEOUT_S)
    p.add_argument("--regress-threshold", type=int, default=DEFAULT_REGRESS_THRESHOLD)
    p.add_argument("--report-interval", type=float, default=1.0)
    p.add_argument("--duration", type=float, default=None, help="N초 뒤 종료하고 요약 출력")
    p.add_argument("--verbose", action="store_true", help="패킷마다 출력")
    p.add_argument("--json", action="store_true", help="종료 시 요약을 JSON 한 줄로도 출력")
    p.add_argument("--quiet", action="store_true", help="주기 출력 생략 (요약만)")
    return p


def main(argv: list[str] | None = None) -> int:
    a = build_parser().parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")  # cp949 when piped on Korean Windows
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        try:
            sock.bind((a.host, a.port))
        except OSError as e:
            print(
                f"{a.host}:{a.port} 바인딩 실패: {e}\n"
                "  같은 포트를 쓰는 프로세스(Unreal PIE, 다른 수신 검사기)가 실행 중인지 확인해 주세요",
                file=sys.stderr,
            )
            return 2
        sock.settimeout(0.05)
        print(f"수신 대기 {a.host}:{a.port}: 기대 스키마 {EXPECTED_ADDRESS} {EXPECTED_TYPE_TAGS} ({EXPECTED_SIZE}B)")
        checker = Checker(a.idle_timeout, a.regress_threshold, a.verbose)
        start = time.monotonic()
        next_report = start + a.report_interval
        try:
            while True:
                now = time.monotonic()
                if a.duration is not None and now - start >= a.duration:
                    break
                try:
                    data, _ = sock.recvfrom(65535)
                    checker.on_datagram(data, time.monotonic())
                except socket.timeout:
                    pass
                except ConnectionResetError:
                    # Windows reports ICMP port-unreachable on UDP sockets as WSAECONNRESET; harmless here.
                    pass
                now = time.monotonic()
                checker.on_tick(now)
                if now >= next_report:
                    if not a.quiet:
                        print(checker.window_line(now - start, a.report_interval))
                    next_report += a.report_interval
        except KeyboardInterrupt:
            print("Ctrl+C: 종료")
        s = checker.summary(time.monotonic() - start)
        print(
            f"요약: 수신 {s.received} 채택 {s.accepted} 유실 {s.lost} ({100 * s.loss_rate:.1f}%) "
            f"평균 {s.rate_hz:.1f}Hz 중복 {s.duplicate} 역순 {s.stale} 재시작/초기화 {s.resets} "
            f"형식오류 {s.malformed} 값경고 {s.value_warnings} Idle {s.idle_events}회 "
            f"jitter L {s.jitter_left:.4f} R {s.jitter_right:.4f}"
        )
        if a.json:
            print("JSON " + json.dumps(_json_safe(asdict(s)), ensure_ascii=False))
        return 0
    finally:
        sock.close()


if __name__ == "__main__":
    sys.exit(main())
