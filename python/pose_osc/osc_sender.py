"""OSC message ``/dstrict/hands`` (DESIGN_DECISIONS.md D3) and the UDP sender.

Type tags are set explicitly instead of letting python-osc infer them from Python types: an int above
2**31 - 1 would silently become int64 (``h``) and a numpy scalar is not a ``float``, either of which would
break the fixed schema ``,iiffffffff`` that the receiver validates.
"""

from __future__ import annotations

import logging
import math
import socket
import time
from dataclasses import dataclass
from types import TracebackType

from pythonosc.osc_message_builder import OscMessageBuilder

from pose_osc.observation import Vec3

log = logging.getLogger(__name__)

OSC_ADDRESS = "/dstrict/hands"
OSC_TYPE_TAGS = ",iiffffffff"
OSC_PACKET_SIZE = 68  # 16 (address) + 12 (type tags) + 10 * 4 (args)
SEQ_MODULO = 2**31  # seq stays a non-negative int32; wrap-around looks like a restart to the receiver (D3).

_ZERO: Vec3 = (0.0, 0.0, 0.0)


def _clamp01(v: float) -> float:
    return 0.0 if v < 0.0 else 1.0 if v > 1.0 else v


def _finite(v: float) -> float:
    return v if math.isfinite(v) else 0.0


@dataclass(frozen=True, slots=True)
class HandState:
    position: Vec3
    confidence: float

    @staticmethod
    def absent() -> HandState:
        return HandState(_ZERO, 0.0)

    def sanitized(self) -> HandState:
        """Enforce the schema ranges: x, y, confidence in [0, 1]; z unbounded but finite."""
        x, y, z = (_finite(c) for c in self.position)
        conf = _clamp01(_finite(self.confidence))
        return HandState((_clamp01(x), _clamp01(y), z), conf)


@dataclass(frozen=True, slots=True)
class HandsMessage:
    seq: int
    person: bool
    left: HandState
    right: HandState

    def __post_init__(self) -> None:
        if not 0 <= self.seq < SEQ_MODULO:
            raise ValueError(f"seq must be in [0, 2**31) (got {self.seq})")

    @staticmethod
    def heartbeat(seq: int) -> HandsMessage:
        """No person: same period, person = 0, confidence 0, coordinates 0 (D3)."""
        return HandsMessage(seq, False, HandState.absent(), HandState.absent())

    def encode(self) -> bytes:
        left, right = self.left.sanitized(), self.right.sanitized()
        b = OscMessageBuilder(address=OSC_ADDRESS)
        b.add_arg(self.seq, OscMessageBuilder.ARG_TYPE_INT)
        b.add_arg(1 if self.person else 0, OscMessageBuilder.ARG_TYPE_INT)
        for hand in (left, right):
            for c in hand.position:
                b.add_arg(float(c), OscMessageBuilder.ARG_TYPE_FLOAT)
            b.add_arg(float(hand.confidence), OscMessageBuilder.ARG_TYPE_FLOAT)
        return b.build().dgram


class OscSender:
    """Owns the UDP socket; use as a context manager so the socket is always closed."""

    _WARN_INTERVAL_S = 5.0

    def __init__(self, host: str, port: int) -> None:
        self._address = (host, port)
        self._sock: socket.socket | None = None
        self._last_warn = -math.inf
        self.sent = 0
        self.errors = 0

    def __enter__(self) -> OscSender:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        log.info("OSC 송신 대상 %s:%d (%s, %s)", *self._address, OSC_ADDRESS, OSC_TYPE_TAGS)
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None
            log.info("OSC 소켓을 닫았어요 (송신 %d, 오류 %d)", self.sent, self.errors)

    def send(self, message: HandsMessage) -> bool:
        if self._sock is None:
            raise RuntimeError("OscSender is not open; use it as a context manager")
        try:
            self._sock.sendto(message.encode(), self._address)
        except OSError as e:
            self.errors += 1
            now = time.monotonic()
            if now - self._last_warn >= self._WARN_INTERVAL_S:
                self._last_warn = now
                log.warning("OSC 송신 실패 (%s): 누적 %d회. 계속 시도해요", e, self.errors)
            return False
        self.sent += 1
        return True
