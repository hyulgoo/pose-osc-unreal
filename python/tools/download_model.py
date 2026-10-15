"""Download a MediaPipe Pose Landmarker model into python/models/ (model files are not committed, D10).

URLs are pinned to version 1 of each model and verified by size and SHA-256 (recorded 2026-10-15).
Standard library only.

Usage:
  python tools/download_model.py [--variant lite|full|heavy] [--force]
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path

MODELS_DIR = Path(__file__).resolve().parent.parent / "models"
_BASE = "https://storage.googleapis.com/mediapipe-models/pose_landmarker"


@dataclass(frozen=True, slots=True)
class ModelInfo:
    size: int
    sha256: str

    def url(self, variant: str) -> str:
        return f"{_BASE}/pose_landmarker_{variant}/float16/1/pose_landmarker_{variant}.task"


MODELS = {
    "lite": ModelInfo(5_777_746, "59929e1d1ee95287735ddd833b19cf4ac46d29bc7afddbbf6753c459690d574a"),
    "full": ModelInfo(9_398_198, "5134a3aad27a58b93da0088d431f366da362b44e3ccfbe3462b3827a839011b1"),
    "heavy": ModelInfo(30_664_242, "64437af838a65d18e5ba7a0d39b465540069bc8aae8308de3e318aad31fcbc7b"),
}


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _verify(path: Path, info: ModelInfo) -> str | None:
    """None if OK, otherwise the reason."""
    size = path.stat().st_size
    if size != info.size:
        return f"크기 {size} != {info.size}"
    digest = _sha256(path)
    if digest != info.sha256:
        return f"SHA-256 {digest} != {info.sha256}"
    return None


def download(variant: str, force: bool = False) -> Path:
    info = MODELS[variant]
    target = MODELS_DIR / f"pose_landmarker_{variant}.task"
    if target.exists() and not force:
        problem = _verify(target, info)
        if problem is None:
            print(f"이미 있어요 (검증 통과): {target}")
            return target
        print(f"기존 파일 검증 실패 ({problem}): 다시 받아요")

    MODELS_DIR.mkdir(parents=True, exist_ok=True)
    url = info.url(variant)
    print(f"다운로드: {url}")
    # Download next to the target, verify, then replace atomically: a partial download never looks valid.
    fd, tmp_name = tempfile.mkstemp(prefix=f".{variant}-", suffix=".part", dir=MODELS_DIR)
    tmp = Path(tmp_name)
    try:
        with open(fd, "wb") as out, urllib.request.urlopen(url, timeout=60) as resp:
            while chunk := resp.read(1 << 20):
                out.write(chunk)
        problem = _verify(tmp, info)
        if problem is not None:
            raise RuntimeError(f"다운로드한 파일 검증 실패: {problem}")
        tmp.replace(target)
    finally:
        tmp.unlink(missing_ok=True)
    print(f"저장했어요 (크기·SHA-256 검증 통과): {target}")
    return target


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="MediaPipe Pose Landmarker 모델 다운로드")
    p.add_argument("--variant", choices=sorted(MODELS), default="lite")
    p.add_argument("--force", action="store_true", help="이미 있어도 다시 받기")
    a = p.parse_args(argv)
    try:
        download(a.variant, a.force)
    except (OSError, RuntimeError) as e:
        print(f"실패: {e}\n  네트워크·프록시를 확인하거나, 위 URL을 브라우저로 받아 {MODELS_DIR}에 넣어 주세요", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
