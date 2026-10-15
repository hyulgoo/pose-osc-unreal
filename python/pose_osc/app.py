"""Main loop: source -> (noise) -> per-hand filter -> OSC send, with fault injection and periodic stats."""

from __future__ import annotations

import logging
import platform
import sys
import time
from contextlib import ExitStack
from dataclasses import dataclass, field
from types import TracebackType
from typing import TYPE_CHECKING, Protocol

import numpy as np

from pose_osc import __version__
from pose_osc.config import AppConfig, FilterConfig, SourceKind, parse_args
from pose_osc.faults import FaultInjector
from pose_osc.hand_filter import HandFilter
from pose_osc.keys import KeyPoller
from pose_osc.metrics import JitterMeter, WindowStat
from pose_osc.observation import ConfidenceSource, HandObservation, PoseObservation
from pose_osc.osc_sender import SEQ_MODULO, HandsMessage, HandState, OscSender
from pose_osc.winqos import disable_power_throttling

if TYPE_CHECKING:
    from pose_osc.capture import CaptureStats, LatestFrameCapture
    from pose_osc.pose import PoseEstimator

log = logging.getLogger("pose_osc")

EXIT_OK = 0
EXIT_SETUP_ERROR = 2
_NO_FRAME_WARN_S = 1.0
_POLL_TIMEOUT_S = 0.1  # Short so Ctrl+C and keys stay responsive (Condition.wait is not interruptible on Windows).


class ObservationSource(Protocol):
    def next_observation(self, timeout_s: float) -> PoseObservation | None: ...


# ----- per-hand processing ---------------------------------------------------------------------------------
@dataclass(slots=True)
class _HandChannel:
    filter: HandFilter
    jitter_raw: JitterMeter = field(default_factory=JitterMeter)
    jitter_filtered: JitterMeter = field(default_factory=JitterMeter)
    visibility: WindowStat = field(default_factory=WindowStat)
    presence: WindowStat = field(default_factory=WindowStat)

    def break_chains(self) -> None:
        self.jitter_raw.break_chain()
        self.jitter_filtered.break_chain()


class HandsPipeline:
    """Noise injection -> gate/reset/One Euro per hand -> values to send. Both raw and filtered are always
    computed so the filter can be toggled at runtime and their jitter compared side by side (D11)."""

    def __init__(self, cfg: FilterConfig, conf_source: ConfidenceSource, faults: FaultInjector) -> None:
        self.filter_enabled = cfg.enabled
        self._conf_source = conf_source
        self._faults = faults
        self.left = _HandChannel(HandFilter(cfg.euro, cfg.gate))
        self.right = _HandChannel(HandFilter(cfg.euro, cfg.gate))

    def process(self, obs: PoseObservation) -> tuple[bool, HandState, HandState]:
        if not obs.person:
            self.left.break_chains()
            self.right.break_chains()
            return False, HandState.absent(), HandState.absent()
        t = obs.timestamp_s
        return True, self._hand(self.left, obs.left, t), self._hand(self.right, obs.right, t)

    def _hand(self, ch: _HandChannel, hand: HandObservation | None, t: float) -> HandState:
        if hand is None:
            ch.break_chains()
            return HandState.absent()
        ch.visibility.add(hand.visibility)
        ch.presence.add(hand.presence)

        raw = self._faults.add_noise(hand.position)
        conf = hand.confidence(self._conf_source)
        result = ch.filter.update(raw, conf, t)
        if result.accepted:
            if result.reset:
                ch.break_chains()
            ch.jitter_raw.add(raw)
            assert result.filtered is not None
            ch.jitter_filtered.add(result.filtered)
        else:
            ch.break_chains()

        use_filtered = self.filter_enabled and result.filtered is not None
        return HandState(result.filtered if use_filtered else raw, conf)  # type: ignore[arg-type]


# ----- camera source ---------------------------------------------------------------------------------------
class CameraPoseSource:
    """Latest-frame capture thread + Pose Landmarker (D1, D2)."""

    def __init__(self, cfg: AppConfig) -> None:
        self._cfg = cfg
        self._stack = ExitStack()
        self._capture: LatestFrameCapture | None = None
        self._estimator: PoseEstimator | None = None
        self._last_index = 0
        self._period = 1.0 / cfg.max_rate_hz if cfg.max_rate_hz else 0.0
        self._next_due = 0.0
        self.last_image: np.ndarray | None = None
        self.infer_ms = WindowStat()
        self.overwritten = 0

    def __enter__(self) -> CameraPoseSource:
        # Imported here so the synthetic source and the tests run without loading OpenCV/MediaPipe.
        from pose_osc.capture import LatestFrameCapture
        from pose_osc.pose import PoseEstimator

        try:
            # Load the model before turning the camera on: a missing model file fails fast.
            self._estimator = self._stack.enter_context(PoseEstimator(self._cfg.pose))
            self._capture = self._stack.enter_context(LatestFrameCapture(self._cfg.camera))
        except BaseException:
            self._stack.close()
            raise
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self._stack.close()

    def next_observation(self, timeout_s: float) -> PoseObservation | None:
        assert self._capture is not None and self._estimator is not None
        if self._period:
            # Rate cap: wait for the next tick, then take the newest frame (older ones stay overwritten).
            wait = self._next_due - time.monotonic()
            if wait > timeout_s:
                time.sleep(timeout_s)
                return None
            if wait > 0.0:
                time.sleep(wait)
        frame = self._capture.wait_next(self._last_index, timeout_s)
        if frame is None:
            return None
        if self._period:
            now = time.monotonic()
            # Fixed cadence; resync after a stall instead of bursting to catch up.
            self._next_due = self._next_due + self._period if now - self._next_due < self._period else now + self._period
        if self._last_index:
            self.overwritten += frame.index - self._last_index - 1
        self._last_index = frame.index
        t0 = time.perf_counter()
        obs = self._estimator.estimate(frame)
        self.infer_ms.add((time.perf_counter() - t0) * 1000.0)
        self.last_image = frame.image
        return obs

    def capture_stats(self) -> CaptureStats:
        assert self._capture is not None
        return self._capture.stats()


# ----- preview window --------------------------------------------------------------------------------------
class Preview:
    WINDOW = "pose-osc preview (q: quit, f: filter)"
    MAX_WIDTH = 960

    def __enter__(self) -> Preview:
        import cv2

        self._cv2 = cv2
        cv2.namedWindow(self.WINDOW, cv2.WINDOW_AUTOSIZE)
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        try:
            self._cv2.destroyWindow(self.WINDOW)
            self._cv2.waitKey(1)
        except self._cv2.error:
            pass

    def show(self, image: np.ndarray | None, left: HandState, right: HandState, status: str) -> list[str]:
        cv2 = self._cv2
        # Display mirrored like a mirror; the sent coordinates are already mirrored (x' = 1 - x), so the
        # markers should sit on the hands. "L" must follow the person's left hand (D1 label check).
        if image is None:
            canvas = np.zeros((360, 640, 3), np.uint8)
        else:
            # Shrink first: flipping and blitting a full 1080p frame every tick halved the send rate (T-004).
            h0, w0 = image.shape[:2]
            if w0 > self.MAX_WIDTH:
                image = cv2.resize(image, (self.MAX_WIDTH, round(h0 * self.MAX_WIDTH / w0)), interpolation=cv2.INTER_AREA)
            canvas = cv2.flip(image, 1)
        h, w = canvas.shape[:2]
        for label, hand in (("L", left), ("R", right)):
            if hand.confidence <= 0.0:
                continue
            color = (0, 200, 0) if hand.confidence >= 0.6 else (0, 200, 255) if hand.confidence >= 0.4 else (0, 0, 255)
            cx, cy = int(hand.position[0] * w), int(hand.position[1] * h)
            cv2.circle(canvas, (cx, cy), 14, color, 3)
            cv2.putText(canvas, f"{label} {hand.confidence:.2f}", (cx + 18, cy + 6), cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2)
        cv2.putText(canvas, status, (10, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 255, 255), 2)
        cv2.imshow(self.WINDOW, canvas)

        keys: list[str] = []
        code = cv2.waitKey(1) & 0xFF
        if code == 27:
            keys.append("q")
        elif code != 0xFF:
            keys.append(chr(code).lower())
        if cv2.getWindowProperty(self.WINDOW, cv2.WND_PROP_VISIBLE) < 1:
            keys.append("q")
        return keys


# ----- main loop -------------------------------------------------------------------------------------------
@dataclass(slots=True)
class _WindowCounters:
    started: float
    processed: int = 0
    person: int = 0
    sent: int = 0
    dropped: int = 0
    frozen: int = 0
    latency_ms: WindowStat = field(default_factory=WindowStat)
    captured_at_start: int = 0
    overwritten_at_start: int = 0


def _fmt(v: float, spec: str = ".4f") -> str:
    return "-" if v != v else format(v, spec)  # NaN -> "-"


class Runner:
    def __init__(self, cfg: AppConfig, source: ObservationSource, sender: OscSender, preview: Preview | None) -> None:
        self._cfg = cfg
        self._source = source
        self._sender = sender
        self._preview = preview
        self._faults = FaultInjector(cfg.faults)
        self._pipeline = HandsPipeline(cfg.filter, cfg.pose.confidence_source, self._faults)
        self._keys = KeyPoller()
        self._seq = 0
        self._frozen = False

    def run(self) -> int:
        cfg = self._cfg
        start = time.monotonic()
        last_obs_t = start
        no_frame = False
        window = self._new_window(start)
        if self._keys.available:
            log.info("키: f = 필터 토글, q = 종료 (Ctrl+C도 가능)")

        while True:
            now = time.monotonic()
            if cfg.duration_s is not None and now - start >= cfg.duration_s:
                log.info("--duration %.1fs 경과: 종료해요", cfg.duration_s)
                return EXIT_OK
            if self._handle_keys(self._keys.poll()):
                return EXIT_OK

            obs = self._source.next_observation(_POLL_TIMEOUT_S)
            now = time.monotonic()
            if obs is None:
                if not no_frame and now - last_obs_t > _NO_FRAME_WARN_S:
                    no_frame = True
                    log.warning("새 프레임이 %.1f초째 없어요: 송신을 멈춘 상태예요 (수신 측은 Idle로 전환돼요)", now - last_obs_t)
            else:
                if no_frame:
                    log.info("프레임 재개 (%.1f초 공백): 송신 재개", now - last_obs_t)
                    no_frame = False
                last_obs_t = now
                if self._step(obs, now, window):
                    return EXIT_OK

            if now - window.started >= cfg.stats_interval_s:
                self._log_stats(window, now)
                window = self._new_window(now)

    def _step(self, obs: PoseObservation, now: float, window: _WindowCounters) -> bool:
        person, left, right = self._pipeline.process(obs)
        window.processed += 1
        window.person += int(person)

        frozen = self._faults.is_frozen(now)
        if frozen != self._frozen:
            self._frozen = frozen
            if frozen:
                log.warning("[장애 주입] 송신 동결 시작: 다음 seq %d (동결 중에는 seq가 증가하지 않아요)", self._seq)
            else:
                log.warning("[장애 주입] 송신 동결 해제: seq %d부터 재개", self._seq)

        if frozen:
            window.frozen += 1
        else:
            message = HandsMessage(self._seq, person, left, right)
            self._seq = (self._seq + 1) % SEQ_MODULO
            if self._faults.should_drop():
                window.dropped += 1
            elif self._sender.send(message):
                window.sent += 1
                window.latency_ms.add((time.monotonic() - obs.timestamp_s) * 1000.0)

        if self._preview is not None:
            image = getattr(self._source, "last_image", None)
            status = f"filter {'ON' if self._pipeline.filter_enabled else 'OFF'}  seq {self._seq}" + ("  FROZEN" if frozen else "")
            return self._handle_keys(self._preview.show(image, left, right, status))
        return False

    def _handle_keys(self, keys: list[str]) -> bool:
        for k in keys:
            if k == "q":
                log.info("q 입력: 종료해요")
                return True
            if k == "f":
                self._pipeline.filter_enabled = not self._pipeline.filter_enabled
                log.info("필터 %s (raw/필터 지터는 통계 로그에서 계속 비교돼요)", "ON" if self._pipeline.filter_enabled else "OFF")
        return False

    def _new_window(self, now: float) -> _WindowCounters:
        w = _WindowCounters(started=now)
        if isinstance(self._source, CameraPoseSource):
            w.captured_at_start = self._source.capture_stats().captured
            w.overwritten_at_start = self._source.overwritten
            self._source.infer_ms.reset()
        for ch in (self._pipeline.left, self._pipeline.right):
            ch.jitter_raw.reset_window()
            ch.jitter_filtered.reset_window()
            ch.visibility.reset()
            ch.presence.reset()
        return w

    def _log_stats(self, w: _WindowCounters, now: float) -> None:
        el = max(now - w.started, 1e-6)
        parts: list[str] = []
        if isinstance(self._source, CameraPoseSource):
            cs = self._source.capture_stats()
            parts.append(
                f"capture {(cs.captured - w.captured_at_start) / el:.1f}fps"
                f" (덮어씀 {self._source.overwritten - w.overwritten_at_start}, read 실패 누적 {cs.read_failures}"
                f"{', 동일 프레임 중' if cs.stale else ''})"
            )
            inf = self._source.infer_ms
            parts.append(f"infer {_fmt(inf.mean, '.1f')}ms (max {inf.peak:.1f})")
        parts.append(f"송신 {w.sent / el:.1f}Hz (drop {w.dropped}, freeze {w.frozen})")
        parts.append(f"지연 cap→send {_fmt(w.latency_ms.mean, '.1f')}ms (max {w.latency_ms.peak:.1f})")
        parts.append(f"person {100.0 * w.person / w.processed if w.processed else 0.0:.0f}%")
        for name, ch in (("L", self._pipeline.left), ("R", self._pipeline.right)):
            parts.append(
                f"{name} vis {_fmt(ch.visibility.mean, '.2f')} pres {_fmt(ch.presence.mean, '.2f')}"
                f" jitter {_fmt(ch.jitter_raw.mean)}→{_fmt(ch.jitter_filtered.mean)}"
            )
        parts.append(f"filter {'ON' if self._pipeline.filter_enabled else 'OFF'}")
        log.info(" | ".join(parts))


def _make_source(cfg: AppConfig) -> ObservationSource:
    if cfg.source is SourceKind.SYNTHETIC:
        from pose_osc.synthetic import SyntheticSource

        return SyntheticSource(cfg.synthetic)
    return CameraPoseSource(cfg)


def _log_startup(cfg: AppConfig) -> None:
    versions = [f"pose_osc {__version__}", f"Python {platform.python_version()}"]
    try:
        from importlib.metadata import version

        for dist in ("mediapipe", "opencv-contrib-python", "python-osc", "numpy"):
            versions.append(f"{dist} {version(dist)}")
    except Exception:  # noqa: BLE001 - version logging is best effort
        pass
    log.info("시작: %s", ", ".join(versions))
    log.info(
        "source=%s filter=%s(min_cutoff=%.3g beta=%.3g d_cutoff=%.3g gate=%.2f reset_gap=%.2fs) conf=%s",
        cfg.source.value,
        "ON" if cfg.filter.enabled else "OFF",
        cfg.filter.euro.min_cutoff,
        cfg.filter.euro.beta,
        cfg.filter.euro.d_cutoff,
        cfg.filter.gate.min_confidence,
        cfg.filter.gate.reset_gap_s,
        cfg.pose.confidence_source.value,
    )
    f = cfg.faults
    if f.any_enabled:
        log.warning(
            "[장애 주입] drop-rate=%.2f freeze-after=%s freeze-for=%s noise=%.4g seed=%s",
            f.drop_rate,
            f.freeze_after_s,
            f.freeze_for_s,
            f.noise_sigma,
            f.seed,
        )


def run(cfg: AppConfig) -> int:
    _log_startup(cfg)
    if cfg.allow_power_throttling:
        log.info("Windows 전원 스로틀링을 그대로 둬요 (--allow-power-throttling)")
    elif disable_power_throttling():
        log.info("Windows 전원 스로틀링(EcoQoS)을 이 프로세스에서 껐어요 (백그라운드에서도 추론 속도 유지)")
    with ExitStack() as stack:
        try:
            source = stack.enter_context(_make_source(cfg))
        except FileNotFoundError as e:
            log.error("%s", e)
            return EXIT_SETUP_ERROR
        except RuntimeError as e:  # CameraError
            log.error("%s", e)
            return EXIT_SETUP_ERROR
        sender = stack.enter_context(OscSender(cfg.osc.host, cfg.osc.port))
        preview = stack.enter_context(Preview()) if cfg.preview else None
        return Runner(cfg, source, sender, preview).run()


def main(argv: list[str] | None = None) -> int:
    cfg = parse_args(argv)
    # When piped (e.g. `| Tee-Object`), stdout uses the ANSI code page (cp949 on Korean Windows); never let an
    # unencodable character drop a log line.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    logging.basicConfig(
        level=getattr(logging, cfg.log_level),
        format="%(asctime)s.%(msecs)03d %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
        stream=sys.stdout,
    )
    try:
        return run(cfg)
    except KeyboardInterrupt:
        # Context managers above have already released the camera, landmarker and socket.
        log.info("Ctrl+C: 종료했어요")
        return EXIT_OK
