import pytest

from swlink.protocol import Decoder, encode


def test_encode_plain_line():
    assert encode({"t": "hello"}) == b'{"t":"hello"}\n'


def test_encode_chunk_sets_n():
    out = encode({"t": "spk.pcm", "rate": 16000}, b"\x00\x01\x02\x03")
    assert out == b'{"t":"spk.pcm","rate":16000,"n":4}\n\x00\x01\x02\x03'


def test_decode_lines_split_across_feeds():
    d = Decoder()
    assert d.feed(b'{"t":"btn","id":"A",') == []
    msgs = d.feed(b'"ev":"click"}\n{"t":"power","bat":9}\n')
    assert [m.obj["t"] for m in msgs] == ["btn", "power"]
    assert msgs[0].obj["ev"] == "click"


def test_decode_chunk_waits_for_all_bytes():
    d = Decoder()
    header = b'{"t":"mic","seq":0,"n":6,"rate":16000,"fmt":"s16le"}\n'
    assert d.feed(header + b"abc") == []
    msgs = d.feed(b"def" + b'{"t":"ack","of":"mic.start"}\n')
    assert msgs[0].t == "mic" and msgs[0].data == b"abcdef"
    assert msgs[1].t == "ack"


def test_binary_payload_containing_newline_and_braces_does_not_desync():
    d = Decoder()
    payload = b'\n{"t":"fake"}\n\x00'
    msgs = d.feed(encode({"t": "mic", "seq": 1, "rate": 8000}, payload) + b'{"t":"log","msg":"x"}\n')
    assert [m.t for m in msgs] == ["mic", "log"]
    assert msgs[0].data == payload


def test_invalid_json_reported_not_raised():
    d = Decoder()
    msgs = d.feed(b"garbage\r\n\n{\"t\":\"ok\"}\n")
    assert msgs[0].t == "_bad" and msgs[0].obj["raw"] == "garbage"
    assert msgs[1].t == "ok"


def test_missing_t_is_bad():
    d = Decoder()
    assert d.feed(b'{"x":1}\n')[0].t == "_bad"


def test_overlong_line_rejected_on_encode():
    with pytest.raises(ValueError):
        encode({"t": "display.text", "text": "x" * 600})
