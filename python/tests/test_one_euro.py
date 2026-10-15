import math
import random

import pytest

from pose_osc.one_euro import OneEuroFilter, OneEuroParams, smoothing_factor

DT = 1.0 / 30.0


def run(f: OneEuroFilter, xs: list[float], dt: float = DT, t0: float = 0.0) -> list[float]:
    return [f(x, t0 + i * dt) for i, x in enumerate(xs)]


def test_first_sample_passes_through() -> None:
    f = OneEuroFilter()
    assert f(0.42, 10.0) == 0.42
    assert f.initialized


def test_matches_hand_computed_values() -> None:
    # beta = 0 -> plain low-pass at min_cutoff: alpha = 1 / (1 + tau/dt), tau = 1/(2*pi*1 Hz)
    alpha = 1.0 / (1.0 + (1.0 / (2.0 * math.pi)) / DT)
    assert smoothing_factor(DT, 1.0) == pytest.approx(alpha)
    assert alpha == pytest.approx(0.1731707, abs=1e-7)  # 1 / (1 + 30 / (2*pi))

    f = OneEuroFilter(OneEuroParams(min_cutoff=1.0, beta=0.0))
    out = run(f, [0.0, 1.0, 1.0])
    assert out[0] == 0.0
    assert out[1] == pytest.approx(alpha)
    assert out[2] == pytest.approx(alpha + (1 - alpha) * alpha)


def test_constant_input_stays_constant() -> None:
    f = OneEuroFilter(OneEuroParams(min_cutoff=1.0, beta=0.5))
    assert all(v == pytest.approx(0.3) for v in run(f, [0.3] * 100))


def test_step_response_is_monotonic_without_overshoot() -> None:
    f = OneEuroFilter(OneEuroParams(min_cutoff=1.0, beta=0.0))
    out = run(f, [0.0] + [1.0] * 90)
    assert all(b >= a for a, b in zip(out, out[1:]))
    assert max(out) <= 1.0
    assert out[-1] > 0.99


def test_reduces_stationary_noise() -> None:
    rng = random.Random(1234)
    noisy = [0.5 + rng.gauss(0.0, 0.01) for _ in range(600)]
    out = run(OneEuroFilter(OneEuroParams(min_cutoff=1.0, beta=0.0)), noisy)

    def std(v: list[float]) -> float:
        m = sum(v) / len(v)
        return math.sqrt(sum((x - m) ** 2 for x in v) / len(v))

    # First-order low-pass on white noise: std ratio sqrt(alpha / (2 - alpha)) ~= 0.31 at 1 Hz / 30 Hz.
    assert std(out[60:]) < 0.4 * std(noisy[60:])


def test_beta_reduces_lag_on_fast_motion() -> None:
    speed = 1.0  # normalized units per second (hand crossing the frame in one second)
    xs = [speed * i * DT for i in range(60)]
    lag0 = xs[-1] - run(OneEuroFilter(OneEuroParams(min_cutoff=1.0, beta=0.0)), xs)[-1]
    lag1 = xs[-1] - run(OneEuroFilter(OneEuroParams(min_cutoff=1.0, beta=1.0)), xs)[-1]
    assert lag0 > 0.1  # ~tau * speed = 0.16 at beta = 0
    assert lag1 < 0.5 * lag0


def test_frame_rate_independent_step_response() -> None:
    # Same continuous signal sampled at 30 Hz and 60 Hz -> nearly the same output at t = 1 s, because the
    # smoothing factor uses the real dt. (A fixed-alpha EMA would differ a lot.)
    def response(rate: float) -> float:
        f = OneEuroFilter(OneEuroParams(min_cutoff=1.0, beta=0.0))
        n = int(rate)
        out = 0.0
        for i in range(n + 1):
            t = i / rate
            out = f(1.0 if t >= 0.5 else 0.0, t)
        return out

    assert response(30.0) == pytest.approx(response(60.0), abs=0.02)


def test_non_increasing_timestamp_keeps_state() -> None:
    f = OneEuroFilter()
    f(0.0, 1.0)
    y = f(1.0, 1.1)
    assert f(5.0, 1.1) == y  # dt == 0
    assert f(5.0, 1.0) == y  # dt < 0
    assert math.isfinite(f(1.0, 1.2))


def test_reset_makes_next_sample_pass_through() -> None:
    f = OneEuroFilter()
    run(f, [0.0] * 10)
    f.reset()
    assert not f.initialized
    assert f(0.9, 5.0) == 0.9


@pytest.mark.parametrize(
    "kwargs",
    [{"min_cutoff": 0.0}, {"min_cutoff": -1.0}, {"d_cutoff": 0.0}, {"beta": -0.1}],
)
def test_invalid_params_raise(kwargs: dict[str, float]) -> None:
    with pytest.raises(ValueError):
        OneEuroParams(**kwargs)
