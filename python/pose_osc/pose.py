"""MediaPipe Pose Landmarker wrapper (DESIGN_DECISIONS.md D1).

Verified against mediapipe 1.1.1: the legacy ``mp.solutions`` API is gone; only the Tasks API remains.
"""

from __future__ import annotations

import logging
from pathlib import Path
from types import TracebackType

import cv2
import mediapipe as mp
from mediapipe.tasks.python import BaseOptions
from mediapipe.tasks.python import vision

from pose_osc.capture import Frame
from pose_osc.config import PoseConfig
from pose_osc.observation import HandObservation, PoseObservation

log = logging.getLogger(__name__)

LEFT_WRIST = int(vision.PoseLandmark.LEFT_WRIST)  # 15, person's left
RIGHT_WRIST = int(vision.PoseLandmark.RIGHT_WRIST)  # 16, person's right


class ModelNotFoundError(FileNotFoundError):
    pass


def _hand(landmark: object) -> HandObservation:
    # Mirror after inference, never flip the image before it (D1: flipping first may swap LEFT/RIGHT).
    x = 1.0 - float(landmark.x or 0.0)  # type: ignore[attr-defined]
    y = float(landmark.y or 0.0)  # type: ignore[attr-defined]
    z = float(landmark.z or 0.0)  # type: ignore[attr-defined]
    visibility = float(landmark.visibility or 0.0)  # type: ignore[attr-defined]
    presence = float(landmark.presence or 0.0)  # type: ignore[attr-defined]
    return HandObservation(x, y, z, visibility, presence)


class PoseEstimator:
    """VIDEO-mode landmarker; one synchronous call per new frame (D2). Use as a context manager."""

    def __init__(self, cfg: PoseConfig) -> None:
        self._cfg = cfg
        self._landmarker: vision.PoseLandmarker | None = None
        self._last_ts_ms = -1

    def __enter__(self) -> PoseEstimator:
        path = Path(self._cfg.model_path)
        if not path.is_file():
            raise ModelNotFoundError(
                f"모델 파일이 없어요: {path}\n"
                "  python tools/download_model.py 를 먼저 실행해 주세요 (--variant full 로 다른 모델)"
            )
        options = vision.PoseLandmarkerOptions(
            base_options=BaseOptions(model_asset_path=str(path)),
            running_mode=vision.RunningMode.VIDEO,
            num_poses=1,  # Single visitor; matches the schema's person 0/1 (D1).
            min_pose_detection_confidence=self._cfg.min_detection_confidence,
            min_pose_presence_confidence=self._cfg.min_presence_confidence,
            min_tracking_confidence=self._cfg.min_tracking_confidence,
            output_segmentation_masks=False,
        )
        self._landmarker = vision.PoseLandmarker.create_from_options(options)
        log.info("Pose Landmarker 준비 완료 (%s, VIDEO 모드, CPU)", path.name)
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        if self._landmarker is not None:
            self._landmarker.close()
            self._landmarker = None
            log.info("Pose Landmarker를 닫았어요")

    def estimate(self, frame: Frame) -> PoseObservation:
        if self._landmarker is None:
            raise RuntimeError("PoseEstimator is not open; use it as a context manager")

        # detect_for_video needs strictly increasing timestamps; capture time in ms is, except after a
        # sub-millisecond gap, so bump by 1 ms when needed.
        ts_ms = max(int(frame.timestamp_s * 1000.0), self._last_ts_ms + 1)
        self._last_ts_ms = ts_ms

        rgb = cv2.cvtColor(frame.image, cv2.COLOR_BGR2RGB)
        result = self._landmarker.detect_for_video(mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb), ts_ms)

        if not result.pose_landmarks:
            return PoseObservation(frame.index, frame.timestamp_s, None, None)
        landmarks = result.pose_landmarks[0]
        return PoseObservation(frame.index, frame.timestamp_s, _hand(landmarks[LEFT_WRIST]), _hand(landmarks[RIGHT_WRIST]))
