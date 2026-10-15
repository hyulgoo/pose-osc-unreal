"""Per-frame pose observations handed from a source (camera + model, or synthetic) to the filter stage."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

Vec3 = tuple[float, float, float]


class ConfidenceSource(str, Enum):
    """Which model output field becomes the hand confidence (DESIGN_DECISIONS.md §9, still open)."""

    VISIBILITY = "visibility"
    MIN = "min"  # min(visibility, presence)


@dataclass(frozen=True, slots=True)
class HandObservation:
    """One wrist landmark, already mirrored (x' = 1 - x, D1). Values are raw model output (not clamped)."""

    x: float
    y: float
    z: float
    visibility: float
    presence: float

    @property
    def position(self) -> Vec3:
        return (self.x, self.y, self.z)

    def confidence(self, source: ConfidenceSource) -> float:
        if source is ConfidenceSource.MIN:
            return min(self.visibility, self.presence)
        return self.visibility


@dataclass(frozen=True, slots=True)
class PoseObservation:
    """Result for one processed frame. ``timestamp_s`` is the capture time on ``time.monotonic()``."""

    frame_index: int
    timestamp_s: float
    left: HandObservation | None
    right: HandObservation | None

    @property
    def person(self) -> bool:
        return self.left is not None or self.right is not None
