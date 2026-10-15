"""Synthetic observation source: deterministic hand motion + scripted events, no camera or model (D11).

Used for in-session verification (OSC loopback, fault injection) and for driving the Unreal state machine
reproducibly in stage 2. One cycle is 12 s (values in seconds from the cycle start):

  0.0 - 3.0  both hands tracked (confidence 0.95)
  3.0 - 4.0  left hand confidence 0.5 (inside the 0.4-0.6 hysteresis band -> Unreal keeps its state)
  4.0 - 6.0  both hands tracked
  6.0 - 7.5  right hand occluded (confidence 0.1 -> Holding, Lost after 0.5 s, back to Tracking at 7.5)
  7.5 - 9.0  both hands tracked
  9.0 - 11.0 no person (heartbeat with person = 0)
  11.0 - 12.0 both hands tracked
"""

from __future__ import annotations

import math
import time
from types import TracebackType

from pose_osc.config import SyntheticConfig
from pose_osc.observation import HandObservation, PoseObservation

CYCLE_S = 12.0
TRACKED_CONF = 0.95
BAND_CONF = 0.5
OCCLUDED_CONF = 0.1
_OMEGA = 2.0 * math.pi / 4.0  # One loop every 4 s.


def _hand(x: float, y: float, conf: float) -> HandObservation:
    return HandObservation(x, y, -0.1, conf, conf)


def observation_at(t: float, frame_index: int, timestamp_s: float, events: bool = True) -> PoseObservation:
    """Pure function of elapsed time ``t`` so tests can sample it without sleeping."""
    phase = t % CYCLE_S if events else 0.0
    if events and 9.0 <= phase < 11.0:
        return PoseObservation(frame_index, timestamp_s, None, None)

    left_conf = BAND_CONF if events and 3.0 <= phase < 4.0 else TRACKED_CONF
    right_conf = OCCLUDED_CONF if events and 6.0 <= phase < 7.5 else TRACKED_CONF
    a = _OMEGA * t
    left = _hand(0.35 + 0.12 * math.cos(a), 0.5 + 0.12 * math.sin(a), left_conf)
    right = _hand(0.65 + 0.12 * math.sin(a), 0.5 + 0.10 * math.sin(2.0 * a), right_conf)
    return PoseObservation(frame_index, timestamp_s, left, right)


class SyntheticSource:
    def __init__(self, cfg: SyntheticConfig) -> None:
        self._cfg = cfg
        self._period = 1.0 / cfg.rate_hz
        self._start: float | None = None
        self._index = 0

    def __enter__(self) -> SyntheticSource:
        self._start = time.monotonic()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self._start = None

    def next_observation(self, timeout_s: float) -> PoseObservation | None:
        if self._start is None:
            raise RuntimeError("SyntheticSource is not open; use it as a context manager")
        # Fixed schedule (no drift): frame n is due at start + n * period.
        due = self._start + self._index * self._period
        wait = due - time.monotonic()
        if wait > timeout_s:
            time.sleep(timeout_s)
            return None
        if wait > 0.0:
            time.sleep(wait)
        now = time.monotonic()
        self._index += 1
        return observation_at(now - self._start, self._index, now, self._cfg.events)
