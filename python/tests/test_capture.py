"""LatestFrameCapture with a fake VideoCapture: latest-frame slot, stale-frame watchdog, open failure."""

from __future__ import annotations

import threading
import time

import numpy as np
import pytest

from pose_osc.capture import CameraError, LatestFrameCapture
from pose_osc.config import CameraConfig


class FakeCapture:
    """~100 fps fake camera. ``mode`` switches between live noise and a frozen placeholder image."""

    def __init__(self, opened: bool = True) -> None:
        self._opened = opened
        self.mode = "live"
        self._rng = np.random.default_rng(0)
        self._placeholder = np.zeros((72, 128, 3), np.uint8)
        self.released = threading.Event()

    def isOpened(self) -> bool:  # noqa: N802 - mirrors cv2.VideoCapture
        return self._opened

    def set(self, prop: int, value: float) -> bool:
        return True

    def get(self, prop: int) -> float:
        return 0.0

    def getBackendName(self) -> str:  # noqa: N802
        return "FAKE"

    def read(self) -> tuple[bool, np.ndarray | None]:
        time.sleep(0.01)
        if self.mode == "live":
            return True, self._rng.integers(0, 255, (72, 128, 3), dtype=np.uint8)
        if self.mode == "frozen":
            return True, self._placeholder.copy()
        return False, None

    def release(self) -> None:
        self.released.set()


def make(fake: FakeCapture, **cfg: float) -> LatestFrameCapture:
    return LatestFrameCapture(CameraConfig(**cfg), factory=lambda index, api: fake)


def test_wait_next_returns_newest_frame_only() -> None:
    fake = FakeCapture()
    with make(fake) as cap:
        first = cap.wait_next(0, 1.0)
        assert first is not None
        time.sleep(0.1)  # ~10 frames arrive meanwhile
        newest = cap.wait_next(first.index, 1.0)
        assert newest is not None and newest.index > first.index + 1  # older ones were overwritten
    assert fake.released.is_set()


def test_identical_frames_stop_publishing_then_recover() -> None:
    fake = FakeCapture()
    with make(fake, stale_timeout_s=0.3) as cap:
        assert cap.wait_next(0, 1.0) is not None
        fake.mode = "frozen"
        time.sleep(0.6)
        assert cap.stats().stale
        frozen_at = cap.wait_next(0, 0.0).index  # type: ignore[union-attr]
        assert cap.wait_next(frozen_at, 0.3) is None  # nothing new while stale -> no messages (D11)

        fake.mode = "live"
        assert cap.wait_next(frozen_at, 1.0) is not None
        assert not cap.stats().stale


def test_read_failure_reopens_camera() -> None:
    fake = FakeCapture()
    with make(fake, reopen_after_s=0.2) as cap:
        last = cap.wait_next(0, 1.0)
        assert last is not None
        fake.mode = "fail"
        time.sleep(0.4)  # > reopen_after_s -> release, then reopen after a 0.5 s backoff
        assert fake.released.is_set()
        fake.mode = "live"
        assert cap.wait_next(last.index, 3.0) is not None
        s = cap.stats()
        assert s.reopen_count == 1 and s.read_failures > 0


def test_open_failure_raises_with_hints() -> None:
    with pytest.raises(CameraError) as e:
        make(FakeCapture(opened=False)).start()
    assert "다른 앱" in str(e.value)
