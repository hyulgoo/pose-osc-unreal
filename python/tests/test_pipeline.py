"""HandsPipeline + synthetic schedule, without sockets or sleeping."""

import pytest

from pose_osc.app import HandsPipeline
from pose_osc.config import FilterConfig
from pose_osc.faults import FaultInjector, FaultParams
from pose_osc.hand_filter import HandGateParams
from pose_osc.observation import ConfidenceSource, HandObservation, PoseObservation
from pose_osc.synthetic import BAND_CONF, OCCLUDED_CONF, TRACKED_CONF, observation_at

DT = 1.0 / 30.0


def pipeline(noise: float = 0.0, enabled: bool = True) -> HandsPipeline:
    return HandsPipeline(
        FilterConfig(enabled=enabled, gate=HandGateParams(0.4, 0.3)),
        ConfidenceSource.VISIBILITY,
        FaultInjector(FaultParams(noise_sigma=noise, seed=1)),
    )


def obs(i: int, x: float, conf: float) -> PoseObservation:
    h = HandObservation(x, 0.5, 0.0, conf, conf)
    return PoseObservation(i, i * DT, h, h)


def test_no_person_sends_zero_heartbeat_values() -> None:
    person, left, right = pipeline().process(PoseObservation(1, 0.0, None, None))
    assert not person
    assert left.position == (0.0, 0.0, 0.0) and left.confidence == 0.0
    assert right.confidence == 0.0


def test_low_confidence_sends_held_filtered_position_with_real_confidence() -> None:
    p = pipeline()
    p.process(obs(0, 0.3, 0.9))
    _, left_ok, _ = p.process(obs(1, 0.3, 0.9))
    _, left_low, _ = p.process(obs(2, 0.9, 0.2))
    assert left_low.position == left_ok.position  # held, not the raw 0.9
    assert left_low.confidence == pytest.approx(0.2)  # receiver decides from the real confidence


def test_filter_toggle_switches_between_raw_and_filtered() -> None:
    p = pipeline(noise=0.05)
    for i in range(30):
        p.process(obs(i, 0.5, 0.9))
    _, filtered, _ = p.process(obs(30, 0.5, 0.9))
    p.filter_enabled = False
    _, raw, _ = p.process(obs(31, 0.5, 0.9))
    assert abs(filtered.position[0] - 0.5) < 0.03  # filter keeps it close to the true value
    # raw carries the injected noise; with sigma 0.05 a deviation this small would be unlikely
    assert raw.position != filtered.position


def test_noise_raises_raw_jitter_and_filter_lowers_it() -> None:
    p = pipeline(noise=0.02)
    for i in range(300):
        p.process(obs(i, 0.5, 0.9))
    assert p.left.jitter_raw.mean > 0.02
    assert p.left.jitter_filtered.mean < 0.3 * p.left.jitter_raw.mean


def test_synthetic_schedule() -> None:
    assert observation_at(1.0, 1, 1.0).left.visibility == TRACKED_CONF
    assert observation_at(3.5, 1, 3.5).left.visibility == BAND_CONF
    assert observation_at(6.5, 1, 6.5).right.visibility == OCCLUDED_CONF
    assert not observation_at(10.0, 1, 10.0).person
    assert observation_at(12.5, 1, 12.5).left.visibility == TRACKED_CONF  # next cycle
    steady = observation_at(10.0, 1, 10.0, events=False)
    assert steady.person and steady.right.visibility == TRACKED_CONF
