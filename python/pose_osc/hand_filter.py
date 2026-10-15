"""Per-hand filtering: confidence gate + reset on re-acquisition + One Euro per axis (D4, D11).

Invariant (D11): ``min_confidence`` must be <= the Unreal Holding threshold (D6, 0.4). Unreal keeps using
coordinates while it stays in Tracking inside the 0.4-0.6 hysteresis band, so every coordinate Unreal can
use has to have gone through the filter.
"""

from __future__ import annotations

from dataclasses import dataclass

from pose_osc.observation import Vec3
from pose_osc.one_euro import OneEuroFilter, OneEuroParams


@dataclass(frozen=True, slots=True)
class HandGateParams:
    min_confidence: float = 0.4  # Samples below this are not fed to the filter.
    reset_gap_s: float = 0.3  # Gap between accepted samples above this resets the filter.

    def __post_init__(self) -> None:
        if not 0.0 <= self.min_confidence <= 1.0:
            raise ValueError(f"min_confidence must be in [0, 1] (got {self.min_confidence})")
        if self.reset_gap_s <= 0.0:
            raise ValueError(f"reset_gap_s must be > 0 (got {self.reset_gap_s})")


@dataclass(frozen=True, slots=True)
class HandFilterResult:
    filtered: Vec3 | None  # Latest filter output (held when this sample was rejected); None until first accept.
    accepted: bool  # This sample passed the gate and updated the filter.
    reset: bool  # The filter was reset before applying this sample.


class HandFilter:
    """Gate + reset + three scalar One Euro filters for one hand."""

    def __init__(self, euro: OneEuroParams, gate: HandGateParams) -> None:
        self._gate = gate
        self._axes = (OneEuroFilter(euro), OneEuroFilter(euro), OneEuroFilter(euro))
        self._last_accept_t: float | None = None
        self._output: Vec3 | None = None

    @property
    def output(self) -> Vec3 | None:
        return self._output

    def update(self, raw: Vec3, confidence: float, t: float) -> HandFilterResult:
        if confidence < self._gate.min_confidence:
            return HandFilterResult(self._output, accepted=False, reset=False)

        reset = self._last_accept_t is None or (t - self._last_accept_t) > self._gate.reset_gap_s
        if reset:
            for axis in self._axes:
                axis.reset()

        fx, fy, fz = self._axes
        self._output = (fx(raw[0], t), fy(raw[1], t), fz(raw[2], t))
        self._last_accept_t = t
        return HandFilterResult(self._output, accepted=True, reset=reset)
