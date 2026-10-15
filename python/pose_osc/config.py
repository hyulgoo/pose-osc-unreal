"""Settings as dataclasses, built from the command line."""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

from pose_osc.faults import FaultParams
from pose_osc.hand_filter import HandGateParams
from pose_osc.observation import ConfidenceSource
from pose_osc.one_euro import OneEuroParams

PYTHON_DIR = Path(__file__).resolve().parent.parent
MODELS_DIR = PYTHON_DIR / "models"
MODEL_VARIANTS = ("lite", "full", "heavy")


def default_model_path(variant: str) -> Path:
    return MODELS_DIR / f"pose_landmarker_{variant}.task"


class SourceKind(str, Enum):
    CAMERA = "camera"
    SYNTHETIC = "synthetic"


class CameraBackend(str, Enum):
    AUTO = "auto"  # OpenCV default (MSMF on Windows)
    MSMF = "msmf"
    DSHOW = "dshow"


@dataclass(frozen=True, slots=True)
class CameraConfig:
    index: int = 0
    width: int = 1280
    height: int = 720
    fps: float = 30.0
    backend: CameraBackend = CameraBackend.AUTO
    fourcc: str | None = None  # e.g. "MJPG"; None keeps the driver default.
    open_timeout_s: float = 10.0
    reopen_after_s: float = 1.0  # Continuous read failure longer than this -> release and reopen.
    stale_timeout_s: float = 1.0  # Byte-identical frames for longer than this -> camera fault (D11). 0 = off.


@dataclass(frozen=True, slots=True)
class PoseConfig:
    model_path: Path = field(default_factory=lambda: default_model_path("lite"))
    min_detection_confidence: float = 0.5
    min_presence_confidence: float = 0.5
    min_tracking_confidence: float = 0.5
    confidence_source: ConfidenceSource = ConfidenceSource.VISIBILITY


@dataclass(frozen=True, slots=True)
class FilterConfig:
    enabled: bool = True  # Initial state; toggled at runtime with the 'f' key.
    euro: OneEuroParams = field(default_factory=OneEuroParams)
    gate: HandGateParams = field(default_factory=HandGateParams)


@dataclass(frozen=True, slots=True)
class OscConfig:
    host: str = "127.0.0.1"
    port: int = 8000


@dataclass(frozen=True, slots=True)
class SyntheticConfig:
    rate_hz: float = 30.0
    events: bool = True  # Scripted occlusion / no-person periods (see synthetic.py).


@dataclass(frozen=True, slots=True)
class AppConfig:
    source: SourceKind = SourceKind.CAMERA
    camera: CameraConfig = field(default_factory=CameraConfig)
    pose: PoseConfig = field(default_factory=PoseConfig)
    filter: FilterConfig = field(default_factory=FilterConfig)
    osc: OscConfig = field(default_factory=OscConfig)
    faults: FaultParams = field(default_factory=FaultParams)
    synthetic: SyntheticConfig = field(default_factory=SyntheticConfig)
    preview: bool = False
    max_rate_hz: float = 0.0  # Camera source processing cap; 0 = as fast as frames and inference allow.
    allow_power_throttling: bool = False  # Windows: keep EcoQoS throttling instead of opting out (T-004).
    duration_s: float | None = None  # None: run until Ctrl+C / 'q'.
    stats_interval_s: float = 2.0
    log_level: str = "INFO"


def _unit_interval(text: str) -> float:
    v = float(text)
    if not 0.0 <= v <= 1.0:
        raise argparse.ArgumentTypeError(f"0~1 사이여야 해요 (입력 {v})")
    return v


def _positive(text: str) -> float:
    v = float(text)
    if v <= 0.0:
        raise argparse.ArgumentTypeError(f"0보다 커야 해요 (입력 {v})")
    return v


def _non_negative(text: str) -> float:
    v = float(text)
    if v < 0.0:
        raise argparse.ArgumentTypeError(f"0 이상이어야 해요 (입력 {v})")
    return v


def _port(text: str) -> int:
    v = int(text)
    if not 1 <= v <= 65535:
        raise argparse.ArgumentTypeError(f"포트는 1~65535예요 (입력 {v})")
    return v


def _fourcc(text: str) -> str:
    if len(text) != 4:
        raise argparse.ArgumentTypeError(f"FOURCC는 4글자예요 (입력 {text!r})")
    return text


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="python -m pose_osc",
        description="웹캠 포즈 추정(MediaPipe) → One Euro → OSC /dstrict/hands 송신",
    )
    p.add_argument("--source", choices=[k.value for k in SourceKind], default=SourceKind.CAMERA.value,
                   help="camera: 웹캠 + Pose Landmarker / synthetic: 카메라·모델 없이 정해진 움직임")

    g = p.add_argument_group("카메라")
    g.add_argument("--camera-index", type=int, default=0)
    g.add_argument("--width", type=int, default=1280)
    g.add_argument("--height", type=int, default=720)
    g.add_argument("--fps", type=_positive, default=30.0)
    g.add_argument("--camera-backend", choices=[b.value for b in CameraBackend], default=CameraBackend.AUTO.value)
    g.add_argument("--fourcc", type=_fourcc, default=None, help="예: MJPG (기본: 드라이버 기본값)")
    g.add_argument("--stale-timeout", type=_non_negative, default=1.0,
                   help="동일 프레임이 이 시간(초) 넘게 이어지면 카메라 장애로 보고 송신 중단 (0 = 끔)")
    g.add_argument("--max-rate", type=_non_negative, default=0.0,
                   help="추론·송신 주기 상한 Hz (0 = 제한 없음). 카메라가 60fps를 줄 때 30으로 고정하는 용도")

    g = p.add_argument_group("포즈 모델")
    g.add_argument("--model", choices=MODEL_VARIANTS, default="lite", help="models/ 아래 기본 파일 선택")
    g.add_argument("--model-path", type=Path, default=None, help="모델 파일 경로 직접 지정 (--model보다 우선)")
    g.add_argument("--min-detection-conf", type=_unit_interval, default=0.5)
    g.add_argument("--min-presence-conf", type=_unit_interval, default=0.5)
    g.add_argument("--min-tracking-conf", type=_unit_interval, default=0.5)
    g.add_argument("--conf-source", choices=[s.value for s in ConfidenceSource],
                   default=ConfidenceSource.VISIBILITY.value, help="손 신뢰도로 쓸 값 (visibility 또는 min(visibility, presence))")

    g = p.add_argument_group("One Euro 필터")
    g.add_argument("--no-filter", action="store_true", help="필터 끈 상태로 시작 (실행 중 f 키로 토글)")
    g.add_argument("--min-cutoff", type=_positive, default=1.0, help="Hz (낮을수록 떨림↓ 지연↑)")
    g.add_argument("--beta", type=_non_negative, default=0.0, help="속도 계수 (높을수록 빠른 동작 지연↓)")
    g.add_argument("--d-cutoff", type=_positive, default=1.0, help="미분 컷오프 Hz (저자 권장: 1 유지)")
    g.add_argument("--filter-min-conf", type=_unit_interval, default=0.4,
                   help="이 신뢰도 미만 샘플은 필터에 넣지 않음 (Unreal Holding 임계값 이하로 유지)")
    g.add_argument("--reset-gap", type=_positive, default=0.3, help="유효 샘플 공백이 이 시간(초)을 넘으면 필터 리셋")

    g = p.add_argument_group("OSC")
    g.add_argument("--host", default="127.0.0.1")
    g.add_argument("--port", type=_port, default=8000)

    g = p.add_argument_group("장애 주입")
    g.add_argument("--drop-rate", type=_unit_interval, default=0.0, help="송신 패킷을 버릴 확률 (seq는 소비됨)")
    g.add_argument("--freeze-after", type=_non_negative, default=None, help="첫 메시지 후 N초 뒤 송신 중단")
    g.add_argument("--freeze-for", type=_positive, default=None, help="중단 지속 시간(초). 생략하면 종료까지 중단")
    g.add_argument("--noise", type=_non_negative, default=0.0, help="필터 앞 좌표에 더할 가우시안 노이즈 σ (정규화 단위)")
    g.add_argument("--seed", type=int, default=None, help="장애 주입 난수 시드")

    g = p.add_argument_group("synthetic 소스")
    g.add_argument("--synthetic-rate", type=_positive, default=30.0)
    g.add_argument("--synthetic-no-events", action="store_true", help="가림·사람 없음 이벤트 없이 계속 추적 상태")

    g = p.add_argument_group("실행")
    g.add_argument("--preview", action="store_true", help="미러링된 카메라 화면에 송신 좌표 표시 (q/ESC 종료, f 필터 토글)")
    g.add_argument("--duration", type=_positive, default=None, help="N초 뒤 자동 종료")
    g.add_argument("--allow-power-throttling", action="store_true",
                   help="Windows 전원 스로틀링(EcoQoS) 해제를 하지 않음 (비교 실험용)")
    g.add_argument("--stats-interval", type=_positive, default=2.0, help="통계 로그 주기(초)")
    g.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    return p


def parse_args(argv: list[str] | None = None) -> AppConfig:
    parser = build_parser()
    a = parser.parse_args(argv)
    if a.freeze_for is not None and a.freeze_after is None:
        parser.error("--freeze-for는 --freeze-after와 함께 써야 해요")
    if a.width <= 0 or a.height <= 0:
        parser.error("--width/--height는 0보다 커야 해요")

    try:
        faults = FaultParams(
            drop_rate=a.drop_rate,
            freeze_after_s=a.freeze_after,
            freeze_for_s=a.freeze_for,
            noise_sigma=a.noise,
            seed=a.seed,
        )
        euro = OneEuroParams(min_cutoff=a.min_cutoff, beta=a.beta, d_cutoff=a.d_cutoff)
        gate = HandGateParams(min_confidence=a.filter_min_conf, reset_gap_s=a.reset_gap)
    except ValueError as e:
        parser.error(str(e))

    return AppConfig(
        source=SourceKind(a.source),
        camera=CameraConfig(
            index=a.camera_index,
            width=a.width,
            height=a.height,
            fps=a.fps,
            backend=CameraBackend(a.camera_backend),
            fourcc=a.fourcc,
            stale_timeout_s=a.stale_timeout,
        ),
        pose=PoseConfig(
            model_path=a.model_path if a.model_path is not None else default_model_path(a.model),
            min_detection_confidence=a.min_detection_conf,
            min_presence_confidence=a.min_presence_conf,
            min_tracking_confidence=a.min_tracking_conf,
            confidence_source=ConfidenceSource(a.conf_source),
        ),
        filter=FilterConfig(enabled=not a.no_filter, euro=euro, gate=gate),
        osc=OscConfig(host=a.host, port=a.port),
        faults=faults,
        synthetic=SyntheticConfig(rate_hz=a.synthetic_rate, events=not a.synthetic_no_events),
        preview=a.preview,
        max_rate_hz=a.max_rate,
        allow_power_throttling=a.allow_power_throttling,
        duration_s=a.duration,
        stats_interval_s=a.stats_interval,
        log_level=a.log_level,
    )
