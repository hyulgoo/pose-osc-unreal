"""Sender (synthetic source) -> localhost UDP -> receive checker, as two real processes.

These are the in-session completion checks for stage 1: ~30 Hz, schema-exact packets, and the three fault
injection options visible on the receiver side.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.e2e

PYTHON_DIR = Path(__file__).resolve().parent.parent
CHECKER = PYTHON_DIR / "tools" / "osc_receive_check.py"


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def run_pair(sender_args: list[str], send_s: float, listen_s: float) -> tuple[dict, str]:
    port = _free_port()
    env = {**os.environ, "PYTHONUTF8": "1"}
    receiver = subprocess.Popen(
        [sys.executable, str(CHECKER), "--port", str(port), "--duration", str(listen_s), "--json", "--quiet"],
        cwd=PYTHON_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        env=env,
    )
    try:
        sender = subprocess.run(
            [sys.executable, "-m", "pose_osc", "--source", "synthetic", "--port", str(port),
             "--duration", str(send_s), "--seed", "42", *sender_args],
            cwd=PYTHON_DIR,
            capture_output=True,
            text=True,
            encoding="utf-8",
            env=env,
            timeout=send_s + 30,
        )
        out, _ = receiver.communicate(timeout=listen_s + 30)
    finally:
        if receiver.poll() is None:
            receiver.kill()
    assert sender.returncode == 0, sender.stdout + sender.stderr
    line = next(l for l in out.splitlines() if l.startswith("JSON "))
    return json.loads(line[5:]), sender.stdout


def test_steady_stream_is_30hz_and_schema_exact() -> None:
    s, _ = run_pair(["--synthetic-no-events"], send_s=3.0, listen_s=3.5)
    assert s["malformed"] == {} and s["value_warnings"] == {}
    assert s["lost"] == 0 and s["duplicate"] == 0 and s["stale"] == 0
    assert s["rate_hz"] == pytest.approx(30.0, abs=1.5)
    assert s["accepted"] >= 80
    assert s["person_ratio"] == 1.0


def test_drop_rate_shows_up_as_loss() -> None:
    s, _ = run_pair(["--synthetic-no-events", "--drop-rate", "0.3"], send_s=4.0, listen_s=4.5)
    assert s["loss_rate"] == pytest.approx(0.3, abs=0.07)
    assert s["malformed"] == {}


def test_freeze_after_goes_idle_and_stays_silent() -> None:
    s, out = run_pair(["--synthetic-no-events", "--freeze-after", "1.0"], send_s=3.5, listen_s=4.0)
    assert 25 <= s["accepted"] <= 35  # about one second of packets
    assert s["idle_events"] >= 1
    assert "송신 동결 시작" in out


def test_freeze_for_resumes_without_counting_loss() -> None:
    s, out = run_pair(
        ["--synthetic-no-events", "--freeze-after", "1.0", "--freeze-for", "1.5"], send_s=4.0, listen_s=4.5
    )
    assert s["idle_events"] >= 1
    assert s["resets"].get("idle_reset", 0) == 1  # receiver re-baselined after Idle (D3 rule 1)
    assert s["lost"] == 0  # seq is not consumed while frozen
    assert "송신 동결 해제" in out


def test_noise_is_visible_and_filter_reduces_it() -> None:
    noisy, _ = run_pair(["--synthetic-no-events", "--noise", "0.02", "--no-filter"], send_s=3.0, listen_s=3.5)
    filtered, _ = run_pair(["--synthetic-no-events", "--noise", "0.02"], send_s=3.0, listen_s=3.5)
    clean, _ = run_pair(["--synthetic-no-events", "--no-filter"], send_s=3.0, listen_s=3.5)
    assert noisy["jitter_left"] > 10 * clean["jitter_left"]
    assert filtered["jitter_left"] < 0.3 * noisy["jitter_left"]


def test_synthetic_events_reach_receiver() -> None:
    # One full 12 s cycle would be slow; 10 s covers band, occlusion and the start of the no-person period.
    s, _ = run_pair([], send_s=10.0, listen_s=10.5)
    assert 0.8 < s["person_ratio"] < 1.0
    assert s["value_warnings"] == {}
