import pytest

from swlink.keymap import Action, KeyBackend, KeyMapper, parse_action


class FakeKeys(KeyBackend):
    def __init__(self):
        self.log = []

    def press(self, key):
        self.log.append(("press", key))

    def release(self, key):
        self.log.append(("release", key))


def test_parse_default_verb_and_aliases():
    assert parse_action("space") == Action("tap", ("space",))
    assert parse_action("command+shift+4") == Action("tap", ("cmd", "shift", "4"))
    assert parse_action("press:right") == Action("press", ("right",))
    assert parse_action("release: Right ") == Action("release", ("Right",))


def test_parse_rejects_bad_verb_and_empty():
    with pytest.raises(ValueError):
        parse_action("smash:space")
    with pytest.raises(ValueError):
        parse_action("tap:")


def test_tap_chord_press_order_and_reverse_release():
    kb = FakeKeys()
    m = KeyMapper({"A.click": "cmd+shift+4"}, kb)
    assert m.handle({"t": "btn", "id": "A", "ev": "click"}) == Action("tap", ("cmd", "shift", "4"))
    assert kb.log == [("press", "cmd"), ("press", "shift"), ("press", "4"),
                      ("release", "4"), ("release", "shift"), ("release", "cmd")]


def test_press_release_pair_follow_button_state():
    kb = FakeKeys()
    m = KeyMapper({"B.down": "press:right", "B.up": "release:right"}, kb)
    m.handle({"t": "btn", "id": "B", "ev": "down"})
    m.handle({"t": "btn", "id": "B", "ev": "up"})
    assert kb.log == [("press", "right"), ("release", "right")]


def test_unmapped_and_non_button_messages_ignored():
    kb = FakeKeys()
    m = KeyMapper({"A.click": "space"}, kb)
    assert m.handle({"t": "btn", "id": "B", "ev": "click"}) is None
    assert m.handle({"t": "imu", "ax": 0}) is None
    assert kb.log == []


def test_from_config_reads_keys_table():
    m = KeyMapper.from_config({"keys": {"A.hold": "esc"}})
    assert m.actions == {"A.hold": Action("tap", ("esc",))}


def test_left_right_modifier_aliases_and_chord():
    assert parse_action("rcmd+space") == Action("tap", ("cmd_r", "space"))
    assert parse_action("ctrl_l+shift_r+a") == Action("tap", ("ctrl_l", "shift_r", "a"))
    assert parse_action("press:lalt") == Action("press", ("alt_l",))


def test_pynput_knows_the_side_specific_names():
    from pynput.keyboard import Key
    for n in ("cmd_l", "cmd_r", "ctrl_l", "ctrl_r", "alt_l", "alt_r", "shift_l", "shift_r"):
        assert hasattr(Key, n)


def test_hold_presses_on_down_and_releases_on_up():
    kb = FakeKeys()
    m = KeyMapper({"B.down": "hold:cmd+right"}, kb, key_repeat=False)
    m.trigger("B.down")
    assert kb.log == [("press", "cmd"), ("press", "right")]
    assert m.trigger("B.up") is None
    assert kb.log[2:] == [("release", "right"), ("release", "cmd")]


def test_hold_autorepeats_non_modifier_keys():
    import time
    kb = FakeKeys()
    m = KeyMapper({"A.down": "hold:shift+left"}, kb, key_repeat=True)
    m.trigger("A.down")
    time.sleep(0.62)                     # past the 500 ms delay + a few repeats
    m.trigger("A.up")
    presses = [k for v, k in kb.log if v == "press"]
    assert presses[:2] == ["shift", "left"]
    assert presses.count("left") >= 3 and presses.count("shift") == 1
    assert kb.log[-2:] == [("release", "left"), ("release", "shift")]
    time.sleep(0.1)
    assert kb.log[-2:] == [("release", "left"), ("release", "shift")]   # repeat stopped


def test_hold_on_non_down_trigger_is_ignored_and_release_all_is_safe():
    kb = FakeKeys()
    m = KeyMapper({"A.single": "hold:x", "B.down": "hold:y"}, kb, key_repeat=False)
    assert m.trigger("A.single") is None and kb.log == []
    m.trigger("B.down"); m.release_all()
    assert kb.log == [("press", "y"), ("release", "y")]


def test_modifier_only_tap_has_a_dwell():
    import time
    kb = FakeKeys()
    m = KeyMapper({"A.single": "alt_r"}, kb)
    t0 = time.monotonic(); m.trigger("A.single"); dt = time.monotonic() - t0
    assert kb.log == [("press", "alt_r"), ("release", "alt_r")]
    assert dt >= 0.07


def test_hold_can_bind_to_the_hold_trigger_and_releases_on_up():
    kb = FakeKeys()
    m = KeyMapper({"A.hold": "hold:alt_r"}, kb, key_repeat=False)
    m.trigger("A.hold")
    assert kb.log == [("press", "alt_r")]
    assert m.trigger("A.up") is None          # consumed by the held action
    assert kb.log == [("press", "alt_r"), ("release", "alt_r")]


def test_single_and_double_are_independent_triggers():
    kb = FakeKeys()
    m = KeyMapper({"A.single": "space", "A.double": "enter"}, kb)
    m.trigger("A.double")
    assert [k for v, k in kb.log if v == "press"] == ["enter"]   # single never fires for a double
