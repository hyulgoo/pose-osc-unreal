import math

import pytest

from pose_osc.faults import FaultInjector, FaultParams


def test_no_faults_by_default() -> None:
    f = FaultInjector(FaultParams())
    assert not f.params.any_enabled
    assert f.add_noise((0.1, 0.2, 0.3)) == (0.1, 0.2, 0.3)
    assert not any(f.should_drop() for _ in range(1000))
    assert not f.is_frozen(0.0) and not f.is_frozen(1e6)


def test_drop_rate_is_respected() -> None:
    f = FaultInjector(FaultParams(drop_rate=0.3, seed=7))
    n = 20000
    rate = sum(f.should_drop() for _ in range(n)) / n
    assert rate == pytest.approx(0.3, abs=0.02)
    assert all(FaultInjector(FaultParams(drop_rate=1.0)).should_drop() for _ in range(100))


def test_freeze_forever_after_delay() -> None:
    f = FaultInjector(FaultParams(freeze_after_s=2.0))
    assert not f.is_frozen(100.0)  # clock starts at the first call
    assert not f.is_frozen(101.9)
    assert f.is_frozen(102.0)
    assert f.is_frozen(1000.0)


def test_freeze_for_resumes() -> None:
    f = FaultInjector(FaultParams(freeze_after_s=1.0, freeze_for_s=0.5))
    assert not f.is_frozen(10.0)
    assert f.is_frozen(11.2)
    assert not f.is_frozen(11.5)


def test_noise_has_requested_sigma() -> None:
    f = FaultInjector(FaultParams(noise_sigma=0.01, seed=3))
    xs = [f.add_noise((0.5, 0.5, 0.0))[0] - 0.5 for _ in range(20000)]
    mean = sum(xs) / len(xs)
    std = math.sqrt(sum((x - mean) ** 2 for x in xs) / len(xs))
    assert mean == pytest.approx(0.0, abs=0.0005)
    assert std == pytest.approx(0.01, rel=0.05)


@pytest.mark.parametrize(
    "kwargs",
    [{"drop_rate": -0.1}, {"drop_rate": 1.5}, {"freeze_after_s": -1.0}, {"freeze_for_s": 0.0}, {"noise_sigma": -0.01}],
)
def test_invalid_params_raise(kwargs: dict[str, float]) -> None:
    with pytest.raises(ValueError):
        FaultParams(**kwargs)
