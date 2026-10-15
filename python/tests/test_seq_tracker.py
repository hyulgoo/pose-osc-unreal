"""D3 seq rules as implemented by the receive checker (the Unreal receiver mirrors these)."""

from osc_receive_check import SeqTracker

DT = 1.0 / 30.0


def test_sequential_and_gap() -> None:
    t = SeqTracker()
    assert t.on_packet(0, 0.0).kind == "first"
    assert t.on_packet(1, DT).kind == "next"
    d = t.on_packet(4, 2 * DT)
    assert d.accepted and d.kind == "gap" and d.lost == 2


def test_duplicate_and_small_regression_are_dropped() -> None:
    t = SeqTracker(regress_threshold=30)
    for i in range(50):
        t.on_packet(i, i * DT)
    assert t.on_packet(49, 50 * DT).kind == "duplicate"
    d = t.on_packet(40, 50 * DT)
    assert not d.accepted and d.kind == "stale"


def test_large_regression_is_a_restart() -> None:
    t = SeqTracker(regress_threshold=30)
    for i in range(100):
        t.on_packet(i, i * DT)
    d = t.on_packet(0, 100 * DT)  # sender restarted quickly, before Idle
    assert d.accepted and d.kind == "regress_reset"
    assert t.on_packet(1, 101 * DT).kind == "next"


def test_restart_after_idle_resets_baseline() -> None:
    # Scenario 2: kill Python -> > 1.0 s silence -> restart from seq 0 must be accepted (§8 #6).
    t = SeqTracker(idle_timeout_s=1.0)
    for i in range(10):
        t.on_packet(i, i * DT)
    assert not t.is_idle(9 * DT + 0.9)
    assert t.is_idle(9 * DT + 1.01)
    d = t.on_packet(0, 9 * DT + 1.5)
    assert d.accepted and d.kind == "idle_reset" and d.lost == 0


def test_fast_restart_with_small_regression_recovers_within_a_second() -> None:
    # Edge case documented in D11: last seq 20, restart within 1 s -> 0..20 look stale and are dropped
    # until the new seq passes 20 (or Idle resets the baseline, whichever comes first).
    t = SeqTracker(idle_timeout_s=1.0, regress_threshold=30)
    for i in range(21):
        t.on_packet(i, i * DT)
    last_accept = 20 * DT
    now = last_accept + 0.2
    seq = 0
    while not t.on_packet(seq, now).accepted:
        seq += 1
        now += DT
    assert now - last_accept <= 1.0 + DT
    assert seq == 21


def test_rejected_packets_do_not_refresh_idle_timer() -> None:
    t = SeqTracker(idle_timeout_s=1.0, regress_threshold=30)
    for i in range(21):
        t.on_packet(i, i * DT)
    last_accept = 20 * DT
    now = last_accept + DT
    while now - last_accept <= 1.0:
        assert not t.on_packet(5, now).accepted  # stale, keeps arriving
        now += DT
    d = t.on_packet(5, now)
    assert d.accepted and d.kind == "idle_reset"


def test_second_sender_is_ignored_until_idle() -> None:
    # T-007: two pose_osc processes on the same port interleave two seq streams.
    a, b = ("127.0.0.1", 5000), ("127.0.0.1", 6000)
    t = SeqTracker(idle_timeout_s=1.0, regress_threshold=30)
    assert t.on_packet(100, 0.0, a).kind == "first"
    d = t.on_packet(3, DT, b)
    assert not d.accepted and d.kind == "other_sender"
    assert t.on_packet(101, 2 * DT, a).kind == "next"  # no fake restart from b's low seq
    # a stops; after Idle, b takes over (same path as a restarted sender with a new source port).
    d = t.on_packet(4, 2 * DT + 1.1, b)
    assert d.accepted and d.kind == "idle_reset"
    assert t.locked_sender == b


def test_int32_wraparound_is_treated_as_restart() -> None:
    t = SeqTracker(regress_threshold=30)
    t.on_packet(2**31 - 2, 0.0)
    t.on_packet(2**31 - 1, DT)
    d = t.on_packet(0, 2 * DT)
    assert d.accepted and d.kind == "regress_reset"
