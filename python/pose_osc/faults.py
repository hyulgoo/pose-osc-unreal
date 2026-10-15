"""Fault injection (DESIGN_DECISIONS.md §6).

- drop:   the message is built (seq consumed) but not sent -> looks like UDP loss on the receiver.
- freeze: no message is built (seq not consumed) -> looks like a stalled pipeline; optionally resumes.
- noise:  Gaussian noise on raw coordinates *before* the filter -> looks like model output jitter.
"""

from __future__ import annotations

import random
from dataclasses import dataclass

from pose_osc.observation import Vec3


@dataclass(frozen=True, slots=True)
class FaultParams:
    drop_rate: float = 0.0
    freeze_after_s: float | None = None  # None: never freeze.
    freeze_for_s: float | None = None  # None: frozen until exit. Only meaningful with freeze_after_s.
    noise_sigma: float = 0.0  # Std-dev in normalized coordinate units.
    seed: int | None = None

    def __post_init__(self) -> None:
        if not 0.0 <= self.drop_rate <= 1.0:
            raise ValueError(f"drop_rate must be in [0, 1] (got {self.drop_rate})")
        if self.freeze_after_s is not None and self.freeze_after_s < 0.0:
            raise ValueError(f"freeze_after_s must be >= 0 (got {self.freeze_after_s})")
        if self.freeze_for_s is not None and self.freeze_for_s <= 0.0:
            raise ValueError(f"freeze_for_s must be > 0 (got {self.freeze_for_s})")
        if self.noise_sigma < 0.0:
            raise ValueError(f"noise_sigma must be >= 0 (got {self.noise_sigma})")

    @property
    def any_enabled(self) -> bool:
        return self.drop_rate > 0.0 or self.freeze_after_s is not None or self.noise_sigma > 0.0


class FaultInjector:
    def __init__(self, params: FaultParams) -> None:
        self._params = params
        self._rng = random.Random(params.seed)
        self._start_t: float | None = None

    @property
    def params(self) -> FaultParams:
        return self._params

    def add_noise(self, v: Vec3) -> Vec3:
        sigma = self._params.noise_sigma
        if sigma <= 0.0:
            return v
        g = self._rng.gauss
        return (v[0] + g(0.0, sigma), v[1] + g(0.0, sigma), v[2] + g(0.0, sigma))

    def should_drop(self) -> bool:
        rate = self._params.drop_rate
        if rate <= 0.0:
            return False
        return rate >= 1.0 or self._rng.random() < rate

    def is_frozen(self, now: float) -> bool:
        """Freeze clock starts at the first call (= first message the pipeline produces)."""
        after = self._params.freeze_after_s
        if after is None:
            return False
        if self._start_t is None:
            self._start_t = now
        elapsed = now - self._start_t
        if elapsed < after:
            return False
        duration = self._params.freeze_for_s
        return duration is None or elapsed < after + duration
