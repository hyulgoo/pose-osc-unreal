"""Small windowed statistics for the periodic log line."""

from __future__ import annotations

import math
from dataclasses import dataclass

from pose_osc.observation import Vec3


@dataclass(slots=True)
class WindowStat:
    count: int = 0
    total: float = 0.0
    peak: float = 0.0

    def add(self, v: float) -> None:
        self.count += 1
        self.total += v
        if v > self.peak:
            self.peak = v

    @property
    def mean(self) -> float:
        return self.total / self.count if self.count else math.nan

    def reset(self) -> None:
        self.count, self.total, self.peak = 0, 0.0, 0.0


class JitterMeter:
    """Mean |p[t] - 2 p[t-1] + p[t-2]| over x, y (second difference).

    Smooth motion has a small second difference while frame-to-frame noise has a large one, so this compares
    raw vs filtered jitter without a motion model. A gap (``break_chain``) restarts the chain.
    """

    __slots__ = ("_p1", "_p2", "_stat")

    def __init__(self) -> None:
        self._p1: Vec3 | None = None
        self._p2: Vec3 | None = None
        self._stat = WindowStat()

    def add(self, p: Vec3) -> None:
        if self._p1 is not None and self._p2 is not None:
            ax = p[0] - 2.0 * self._p1[0] + self._p2[0]
            ay = p[1] - 2.0 * self._p1[1] + self._p2[1]
            self._stat.add(math.hypot(ax, ay))
        self._p2, self._p1 = self._p1, p

    def break_chain(self) -> None:
        self._p1 = self._p2 = None

    @property
    def mean(self) -> float:
        return self._stat.mean

    def reset_window(self) -> None:
        self._stat.reset()
