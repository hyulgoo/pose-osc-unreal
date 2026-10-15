"""Capture thread keeping only the newest frame (DESIGN_DECISIONS.md D2) + camera fault handling (D11).

- The camera is opened, read and released on the capture thread itself, so the VideoCapture object never
  crosses threads (MSMF is COM-based; keeping it on one thread avoids apartment issues).
- Frames are published into a single slot guarded by a Condition; older unread frames are overwritten.
- Read failures longer than ``reopen_after_s`` release the camera and reopen it with backoff.
- Byte-identical frames for longer than ``stale_timeout_s`` are treated as a camera fault and not published:
  some drivers keep returning a "camera off" placeholder image with read() == True (TROUBLESHOOTING T-001).
"""

from __future__ import annotations

import logging
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass
from types import TracebackType

import cv2
import numpy as np

from pose_osc.config import CameraBackend, CameraConfig

log = logging.getLogger(__name__)

_BACKENDS = {
    CameraBackend.AUTO: cv2.CAP_ANY,
    CameraBackend.MSMF: cv2.CAP_MSMF,
    CameraBackend.DSHOW: cv2.CAP_DSHOW,
}
_STALE_SAMPLE_STEP = 16  # Compare every 16th pixel in each axis; enough to tell a live sensor from a still image.
_FIRST_FRAME_TIMEOUT_S = 5.0
_REOPEN_BACKOFF_S = (0.5, 1.0, 2.0, 5.0)
_STALE_LOG_INTERVAL_S = 10.0


class CameraError(RuntimeError):
    pass


@dataclass(frozen=True, slots=True)
class Frame:
    index: int  # Increments per captured frame (gaps = frames overwritten before being consumed).
    timestamp_s: float  # time.monotonic() right after read() returned.
    image: np.ndarray  # BGR, as returned by OpenCV.


@dataclass(frozen=True, slots=True)
class CaptureStats:
    captured: int
    read_failures: int
    reopen_count: int
    stale: bool
    stale_frames: int


def _fourcc_to_str(value: float) -> str:
    code = int(value)
    chars = [chr((code >> (8 * k)) & 0xFF) for k in range(4)]
    text = "".join(chars)
    return text if text.isprintable() else f"0x{code:08X}"


def _open_help(cfg: CameraConfig) -> str:
    return (
        f"카메라 {cfg.index}번을 열지 못했어요 (backend={cfg.backend.value}). 확인해 주세요:\n"
        "  1) 다른 앱(카메라 앱, Teams, Zoom, 브라우저)이 카메라를 쓰고 있지 않은지\n"
        "  2) Windows 설정 > 개인 정보 및 보안 > 카메라 > '데스크톱 앱에서 카메라에 액세스' 허용\n"
        "  3) --camera-index 값 (외장 웹캠은 1번일 수 있어요)\n"
        "  4) 노트북 카메라 차단 키·렌즈 셔터\n"
        "  5) --camera-backend msmf / dshow 로 바꿔 보기"
    )


CaptureFactory = Callable[[int, int], "cv2.VideoCapture"]  # (index, api_preference) -> capture


class LatestFrameCapture:
    def __init__(self, cfg: CameraConfig, factory: CaptureFactory = cv2.VideoCapture) -> None:
        self._cfg = cfg
        self._factory = factory  # Injectable so the stale/failure handling can be tested without a camera.
        self._cond = threading.Condition()
        self._latest: Frame | None = None
        self._stop = threading.Event()
        self._opened = threading.Event()
        self._open_error: CameraError | None = None
        self._thread: threading.Thread | None = None
        self._cap: cv2.VideoCapture | None = None  # Owned by the capture thread.
        # Counters written by the capture thread only; read without the lock (ints are atomic in CPython).
        self._captured = 0
        self._read_failures = 0
        self._reopen_count = 0
        self._stale = False
        self._stale_frames = 0

    # ----- lifecycle -------------------------------------------------------------------------------------
    def __enter__(self) -> LatestFrameCapture:
        self.start()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.stop()

    def start(self) -> None:
        """Open the camera on the capture thread and wait until it delivers a first frame (or fails)."""
        self._thread = threading.Thread(target=self._run, name="capture", daemon=True)
        self._thread.start()
        deadline = time.monotonic() + self._cfg.open_timeout_s + _FIRST_FRAME_TIMEOUT_S
        # Wait in short slices: a long lock wait is not interruptible by Ctrl+C on Windows.
        while not self._opened.wait(0.1):
            if time.monotonic() > deadline:
                self.stop()
                raise CameraError(f"카메라 열기가 {self._cfg.open_timeout_s:.0f}초 안에 끝나지 않았어요.\n" + _open_help(self._cfg))
        if self._open_error is not None:
            self.stop()
            raise self._open_error

    def stop(self) -> None:
        self._stop.set()
        with self._cond:
            self._cond.notify_all()
        if self._thread is not None:
            self._thread.join(timeout=3.0)
            if self._thread.is_alive():
                # read() can block inside the driver; the daemon thread is abandoned at interpreter exit.
                log.warning("캡처 스레드가 3초 안에 끝나지 않았어요 (드라이버 read() 대기 중일 수 있음)")
            self._thread = None

    # ----- consumer API ----------------------------------------------------------------------------------
    def wait_next(self, after_index: int, timeout_s: float) -> Frame | None:
        """Newest frame with index > after_index, waiting up to timeout_s. None on timeout or stop."""
        deadline = time.monotonic() + timeout_s
        with self._cond:
            while not self._stop.is_set():
                frame = self._latest
                if frame is not None and frame.index > after_index:
                    return frame
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    return None
                self._cond.wait(remaining)
        return None

    def stats(self) -> CaptureStats:
        return CaptureStats(self._captured, self._read_failures, self._reopen_count, self._stale, self._stale_frames)

    # ----- capture thread --------------------------------------------------------------------------------
    def _open(self) -> cv2.VideoCapture:
        cfg = self._cfg
        t0 = time.monotonic()
        cap = self._factory(cfg.index, _BACKENDS[cfg.backend])
        if not cap.isOpened():
            cap.release()
            raise CameraError(_open_help(cfg))
        if cfg.fourcc:
            cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*cfg.fourcc))
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, cfg.width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, cfg.height)
        cap.set(cv2.CAP_PROP_FPS, cfg.fps)

        first_deadline = time.monotonic() + _FIRST_FRAME_TIMEOUT_S
        while True:
            ok, image = cap.read()
            if ok and image is not None:
                break
            if time.monotonic() > first_deadline or self._stop.is_set():
                cap.release()
                raise CameraError("카메라는 열렸지만 프레임이 오지 않아요.\n" + _open_help(cfg))
            time.sleep(0.05)

        log.info(
            "카메라 %d번 열림 (%.2fs) backend=%s 요청 %dx%d@%.0f%s → 협상 %.0fx%.0f@%.1f fourcc=%s, 프레임 %s",
            cfg.index,
            time.monotonic() - t0,
            cap.getBackendName(),
            cfg.width,
            cfg.height,
            cfg.fps,
            f" {cfg.fourcc}" if cfg.fourcc else "",
            cap.get(cv2.CAP_PROP_FRAME_WIDTH),
            cap.get(cv2.CAP_PROP_FRAME_HEIGHT),
            cap.get(cv2.CAP_PROP_FPS),
            _fourcc_to_str(cap.get(cv2.CAP_PROP_FOURCC)),
            "x".join(str(d) for d in image.shape),
        )
        log.info("보고된 fps는 드라이버 값이에요. 실제 캡처 fps는 통계 로그의 'capture'를 보세요")
        return cap

    def _run(self) -> None:
        try:
            try:
                self._cap = self._open()
            except CameraError as e:
                self._open_error = e
                return
            except Exception as e:  # noqa: BLE001 - surface any OpenCV error as an open failure
                self._open_error = CameraError(f"카메라 열기 중 예외: {e!r}\n" + _open_help(self._cfg))
                return
            finally:
                self._opened.set()
            self._loop()
        except Exception:  # noqa: BLE001 - keep the process alive; no frames -> no messages (D11)
            log.exception("캡처 스레드가 예외로 멈췄어요: 이후 송신은 중단돼요")
        finally:
            if self._cap is not None:
                self._cap.release()
                self._cap = None
                log.info("카메라를 해제했어요")

    def _loop(self) -> None:
        cfg = self._cfg
        cap = self._cap
        assert cap is not None
        fail_since: float | None = None
        prev_sample: np.ndarray | None = None
        same_since: float | None = None
        last_stale_log = -float("inf")

        while not self._stop.is_set():
            ok, image = cap.read()
            now = time.monotonic()

            if not ok or image is None:
                self._read_failures += 1
                if fail_since is None:
                    fail_since = now
                    log.warning("카메라 read() 실패: 프레임 없이 기다리는 동안 송신은 멈춰요")
                if now - fail_since >= cfg.reopen_after_s:
                    cap.release()
                    self._cap = None
                    reopened = self._reopen_with_backoff()
                    if reopened is None:
                        return
                    self._cap = cap = reopened
                    fail_since = None
                    prev_sample = None
                    same_since = None
                else:
                    self._stop.wait(0.01)
                continue

            if fail_since is not None:
                log.info("카메라 read() 복구 (%.2fs 동안 실패)", now - fail_since)
                fail_since = None

            if cfg.stale_timeout_s > 0.0:
                sample = image[::_STALE_SAMPLE_STEP, ::_STALE_SAMPLE_STEP]
                if prev_sample is not None and np.array_equal(sample, prev_sample):
                    if same_since is None:
                        same_since = now
                    if now - same_since >= cfg.stale_timeout_s:
                        self._stale_frames += 1
                        if not self._stale:
                            self._stale = True
                            last_stale_log = now
                            log.error(
                                "카메라가 %.1f초째 똑같은 프레임만 보내요 (밝기 %.1f): 카메라 장애로 보고 송신을 멈춰요. "
                                "카메라 차단 키·렌즈 셔터·다른 앱 점유를 확인해 주세요",
                                now - same_since,
                                float(np.mean(sample)),
                            )
                        elif now - last_stale_log >= _STALE_LOG_INTERVAL_S:
                            last_stale_log = now
                            log.error("카메라 동일 프레임 상태가 %.0f초째 계속돼요", now - same_since)
                        continue
                else:
                    if self._stale:
                        log.info("카메라 프레임이 다시 바뀌어요: 송신 재개")
                    self._stale = False
                    same_since = None
                prev_sample = sample.copy()

            self._captured += 1
            frame = Frame(self._captured, now, image)
            with self._cond:
                self._latest = frame
                self._cond.notify_all()

    def _reopen_with_backoff(self) -> cv2.VideoCapture | None:
        attempt = 0
        while not self._stop.is_set():
            delay = _REOPEN_BACKOFF_S[min(attempt, len(_REOPEN_BACKOFF_S) - 1)]
            log.warning("카메라를 %.1f초 뒤 다시 열어요 (시도 %d)", delay, attempt + 1)
            if self._stop.wait(delay):
                return None
            try:
                cap = self._open()
            except CameraError as e:
                log.warning("카메라 다시 열기 실패: %s", str(e).splitlines()[0])
                attempt += 1
                continue
            self._reopen_count += 1
            return cap
        return None
