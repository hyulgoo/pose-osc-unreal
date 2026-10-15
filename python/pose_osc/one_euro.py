"""One Euro Filter (Casiez, Roussel, Vogel — CHI 2012), DESIGN_DECISIONS.md D4.

Follows the authors' reference implementation:
    alpha(dt, fc) = 1 / (1 + tau / dt),  tau = 1 / (2 * pi * fc)
    dx_hat = lowpass(dx, d_cutoff)
    cutoff = min_cutoff + beta * |dx_hat|
    x_hat  = lowpass(x, cutoff)

``dt`` comes from the caller's timestamps (capture time), so the filter stays correct when frames are
skipped by the latest-frame capture (D2).
"""

from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class OneEuroParams:
    min_cutoff: float = 1.0  # Hz. Lower -> less jitter, more lag at low speed.
    beta: float = 0.0  # Speed coefficient. Units follow the signal (here: normalized coords / s).
    d_cutoff: float = 1.0  # Hz. Derivative cutoff; authors recommend leaving it at 1 Hz.

    def __post_init__(self) -> None:
        if self.min_cutoff <= 0.0:
            raise ValueError(f"min_cutoff must be > 0 (got {self.min_cutoff})")
        if self.d_cutoff <= 0.0:
            raise ValueError(f"d_cutoff must be > 0 (got {self.d_cutoff})")
        if self.beta < 0.0:
            raise ValueError(f"beta must be >= 0 (got {self.beta})")


def smoothing_factor(dt: float, cutoff: float) -> float:
    """Exponential smoothing factor for a first-order low-pass with cutoff ``cutoff`` Hz over ``dt`` s."""
    tau = 1.0 / (2.0 * math.pi * cutoff)
    return 1.0 / (1.0 + tau / dt)


class OneEuroFilter:
    """Scalar One Euro filter. Use one instance per axis per hand."""

    __slots__ = ("_params", "_x_prev", "_dx_prev", "_t_prev")

    def __init__(self, params: OneEuroParams | None = None) -> None:
        self._params = params or OneEuroParams()
        self._x_prev: float | None = None
        self._dx_prev = 0.0
        self._t_prev = 0.0

    @property
    def params(self) -> OneEuroParams:
        return self._params

    @property
    def initialized(self) -> bool:
        return self._x_prev is not None

    @property
    def value(self) -> float | None:
        """Last filtered value, or None before the first sample."""
        return self._x_prev

    def reset(self) -> None:
        """Forget the history; the next sample passes through unchanged (re-acquisition, D4)."""
        self._x_prev = None
        self._dx_prev = 0.0
        self._t_prev = 0.0

    def __call__(self, x: float, t: float) -> float:
        if self._x_prev is None:
            self._x_prev = x
            self._dx_prev = 0.0
            self._t_prev = t
            return x

        dt = t - self._t_prev
        if dt <= 0.0:
            # Duplicate or out-of-order timestamp: keep the state rather than divide by zero.
            return self._x_prev

        p = self._params
        a_d = smoothing_factor(dt, p.d_cutoff)
        dx = (x - self._x_prev) / dt
        dx_hat = a_d * dx + (1.0 - a_d) * self._dx_prev

        cutoff = p.min_cutoff + p.beta * abs(dx_hat)
        a = smoothing_factor(dt, cutoff)
        x_hat = a * x + (1.0 - a) * self._x_prev

        self._x_prev = x_hat
        self._dx_prev = dx_hat
        self._t_prev = t
        return x_hat
