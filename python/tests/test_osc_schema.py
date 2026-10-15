"""Sender encoding vs. the independent receiver parser (and python-osc's own parser as a third opinion)."""

import struct

import pytest
from pythonosc.osc_message import OscMessage

import osc_receive_check as rx
from pose_osc.osc_sender import OSC_ADDRESS, OSC_PACKET_SIZE, OSC_TYPE_TAGS, SEQ_MODULO, HandsMessage, HandState


def sample_message(seq: int = 123) -> HandsMessage:
    return HandsMessage(
        seq,
        True,
        HandState((0.25, 0.5, -0.125), 0.875),
        HandState((0.75, 0.0625, 0.25), 0.5),
    )


def test_sender_constants_match_spec_literals() -> None:
    assert (OSC_ADDRESS, OSC_TYPE_TAGS, OSC_PACKET_SIZE) == (rx.EXPECTED_ADDRESS, rx.EXPECTED_TYPE_TAGS, rx.EXPECTED_SIZE)


def test_roundtrip_through_independent_parser() -> None:
    data = sample_message().encode()
    assert len(data) == 68
    p = rx.parse_hands(data)
    assert (p.seq, p.person) == (123, 1)
    # Values chosen to be exact in float32.
    assert (p.left.x, p.left.y, p.left.z, p.left.conf) == (0.25, 0.5, -0.125, 0.875)
    assert (p.right.x, p.right.y, p.right.z, p.right.conf) == (0.75, 0.0625, 0.25, 0.5)
    assert rx.value_warnings(p) == []


def test_python_osc_parser_agrees() -> None:
    m = OscMessage(sample_message().encode())
    assert m.address == "/dstrict/hands"
    assert m.params[:2] == [123, 1]
    assert len(m.params) == 10


def test_heartbeat_is_all_zero_with_person_0() -> None:
    p = rx.parse_hands(HandsMessage.heartbeat(5).encode())
    assert p.person == 0
    assert (p.left.x, p.left.y, p.left.z, p.left.conf) == (0.0, 0.0, 0.0, 0.0)
    assert rx.value_warnings(p) == []


def test_out_of_range_values_are_clamped_by_sender() -> None:
    m = HandsMessage(1, True, HandState((-0.2, 1.3, -5.0), 1.7), HandState((float("nan"), 0.5, 0.0), -1.0))
    p = rx.parse_hands(m.encode())
    assert (p.left.x, p.left.y, p.left.z, p.left.conf) == (0.0, 1.0, -5.0, 1.0)
    assert (p.right.x, p.right.conf) == (0.0, 0.0)
    assert rx.value_warnings(p) == []


def test_python_ints_and_numpy_scalars_still_produce_fixed_tags() -> None:
    np = pytest.importorskip("numpy")
    m = HandsMessage(SEQ_MODULO - 1, True, HandState((np.float32(0.5), np.float64(0.5), 0), 1), HandState.absent())
    p = rx.parse_hands(m.encode())  # raises if any tag were not i/f
    assert p.seq == SEQ_MODULO - 1


@pytest.mark.parametrize("seq", [-1, SEQ_MODULO])
def test_seq_outside_int32_is_rejected(seq: int) -> None:
    with pytest.raises(ValueError):
        HandsMessage(seq, False, HandState.absent(), HandState.absent())


# ----- receiver rejects anything that is not exactly D3 ----------------------------------------------------
def _osc_string(s: str) -> bytes:
    b = s.encode() + b"\x00"
    return b + b"\x00" * (-len(b) % 4)


def _message(address: str, tags: str, payload: bytes) -> bytes:
    return _osc_string(address) + _osc_string(tags) + payload


GOOD_PAYLOAD = struct.pack(">ii8f", 1, 1, 0.1, 0.2, 0.3, 0.9, 0.4, 0.5, 0.6, 0.8)


@pytest.mark.parametrize(
    ("data", "kind"),
    [
        (_message("/dstrict/hand", ",iiffffffff", GOOD_PAYLOAD), "address"),
        (_message("/dstrict/hands", ",iiiiiiiiii", GOOD_PAYLOAD), "typetag"),
        (_message("/dstrict/hands", ",iifffffff", GOOD_PAYLOAD[:-4]), "typetag"),  # 9 args
        (_message("/dstrict/hands", ",iiffffffffd", GOOD_PAYLOAD + b"\x00" * 8), "typetag"),
        (_message("/dstrict/hands", ",iiffffffff", GOOD_PAYLOAD[:-1]), "size"),
        (_message("/dstrict/hands", ",iiffffffff", GOOD_PAYLOAD + b"\x00\x00\x00\x00"), "size"),
        (b"#bundle\x00" + b"\x00" * 8, "bundle"),
        (b"/dstrict/hands", "truncated"),
        (b"/dstrict/hands\x00\x01" + _osc_string(",iiffffffff") + GOOD_PAYLOAD, "padding"),
    ],
)
def test_receiver_rejects_schema_violations(data: bytes, kind: str) -> None:
    with pytest.raises(rx.SchemaError) as e:
        rx.parse_hands(data)
    assert e.value.kind == kind


def test_receiver_accepts_hand_built_packet() -> None:
    p = rx.parse_hands(_message("/dstrict/hands", ",iiffffffff", GOOD_PAYLOAD))
    assert p.seq == 1


def test_value_warnings_catch_range_problems() -> None:
    bad = struct.pack(">ii8f", 1, 2, 1.5, 0.2, 0.3, 1.2, 0.4, 0.5, 0.6, 0.8)
    w = rx.value_warnings(rx.parse_hands(_message("/dstrict/hands", ",iiffffffff", bad)))
    assert {"person_not_0_1", "left_xy_range", "left_conf_range"} <= set(w)
    hb = struct.pack(">ii8f", 1, 0, 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
    assert "left_heartbeat_not_zero" in rx.value_warnings(rx.parse_hands(_message("/dstrict/hands", ",iiffffffff", hb)))
