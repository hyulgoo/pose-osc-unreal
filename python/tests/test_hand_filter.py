import pytest

from pose_osc.hand_filter import HandFilter, HandGateParams
from pose_osc.one_euro import OneEuroParams

DT = 1.0 / 30.0


def make(min_conf: float = 0.4, reset_gap: float = 0.3) -> HandFilter:
    return HandFilter(OneEuroParams(min_cutoff=1.0, beta=0.0), HandGateParams(min_conf, reset_gap))


def test_first_accepted_sample_passes_through_and_resets() -> None:
    hf = make()
    r = hf.update((0.2, 0.3, -0.1), 0.9, 0.0)
    assert r.accepted and r.reset
    assert r.filtered == (0.2, 0.3, -0.1)


def test_low_confidence_is_not_fed_and_output_is_held() -> None:
    hf = make()
    hf.update((0.2, 0.2, 0.0), 0.9, 0.0)
    held = hf.update((0.2, 0.2, 0.0), 0.9, DT).filtered
    r = hf.update((0.9, 0.9, 0.0), 0.39, 2 * DT)
    assert not r.accepted
    assert r.filtered == held


def test_before_any_accept_output_is_none() -> None:
    r = make().update((0.5, 0.5, 0.0), 0.1, 0.0)
    assert not r.accepted and r.filtered is None


def test_confidence_equal_to_gate_is_accepted() -> None:
    assert make(min_conf=0.4).update((0.5, 0.5, 0.0), 0.4, 0.0).accepted


def test_short_gap_keeps_filter_state() -> None:
    hf = make(reset_gap=0.3)
    hf.update((0.0, 0.0, 0.0), 0.9, 0.0)
    r = hf.update((1.0, 1.0, 0.0), 0.9, 0.2)  # 0.2 s gap < 0.3 s -> smoothed, not a jump
    assert not r.reset
    assert 0.0 < r.filtered[0] < 1.0


def test_long_gap_resets_to_first_new_sample() -> None:
    # Re-acquisition after a long gap must not sweep from the old position (DESIGN_DECISIONS.md §8 #4).
    hf = make(reset_gap=0.3)
    hf.update((0.0, 0.0, 0.0), 0.9, 0.0)
    hf.update((0.5, 0.5, 0.0), 0.1, 0.1)  # rejected sample does not refresh the gap clock
    r = hf.update((1.0, 1.0, 0.0), 0.9, 0.35)
    assert r.reset
    assert r.filtered == (1.0, 1.0, 0.0)


@pytest.mark.parametrize("kwargs", [{"min_confidence": -0.1}, {"min_confidence": 1.1}, {"reset_gap_s": 0.0}])
def test_invalid_gate_params_raise(kwargs: dict[str, float]) -> None:
    with pytest.raises(ValueError):
        HandGateParams(**kwargs)
