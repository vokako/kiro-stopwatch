from swlink.triggers import ALL_TRIGGERS, ImuGestures, TriggerEngine


class Clock:
    def __init__(self): self.t = 0.0
    def __call__(self): return self.t
    def tick(self, dt): self.t += dt


def imu(ax=0.0, ay=0.0, az=1.0, gx=0.0, gy=0.0, gz=0.0):
    return {"t": "imu", "ax": ax, "ay": ay, "az": az, "gx": gx, "gy": gy, "gz": gz}


def test_button_events_become_triggers_except_click():
    e = TriggerEngine()
    assert e.feed({"t": "btn", "id": "A", "ev": "double"}) == ["A.double"]
    assert e.feed({"t": "btn", "id": "PWR", "ev": "hold"}) == ["PWR.hold"]
    assert e.feed({"t": "btn", "id": "A", "ev": "click"}) == []
    assert e.feed({"t": "power", "bat": 50}) == []


def test_idle_noise_fires_nothing():
    g = ImuGestures(_now=Clock())
    for _ in range(200):
        assert g.feed(imu(0.03, -0.004, 0.999, -0.12, -0.06, 0.05)) == []


def test_shake_needs_alternating_swings_within_window():
    clk = Clock(); g = ImuGestures(_now=clk)
    out = []
    for sign in (1, -1, 1):
        out += g.feed(imu(gx=sign * 400)); clk.tick(0.1)
    assert out == ["imu.shake"]
    # same-direction spinning is not a shake
    clk.tick(2); out = []
    for _ in range(5):
        out += g.feed(imu(gx=400)); clk.tick(0.1)
    assert out == []


def test_tilt_fires_once_and_rearms_after_returning_flat():
    clk = Clock(); g = ImuGestures(_now=clk)
    assert g.feed(imu(ay=-0.8, az=0.6)) == ["imu.tilt_right"]   # right edge down
    clk.tick(1)
    assert g.feed(imu(ay=-0.9, az=0.4)) == []          # still tilted: no repeat
    assert g.feed(imu(ay=-0.1, az=1.0)) == []          # flat again: re-armed
    clk.tick(1)
    assert g.feed(imu(ay=-0.8, az=0.6)) == ["imu.tilt_right"]
    clk.tick(1)
    assert g.feed(imu(ay=0.8, az=0.6)) == ["imu.tilt_left"]    # opposite side fires directly
    clk.tick(1)
    assert g.feed(imu(ax=-0.9, az=0.3)) == ["imu.tilt_toward"] # held upright, screen facing you


def test_face_down_transition_only():
    clk = Clock(); g = ImuGestures(_now=clk)
    assert g.feed(imu(az=1.0)) == []                  # first sample establishes state
    clk.tick(1)
    assert g.feed(imu(az=-0.95)) == ["imu.face_down"]
    clk.tick(1)
    assert g.feed(imu(az=-0.95)) == []
    assert g.feed(imu(az=0.95)) == ["imu.face_up"]


def test_cooldown_suppresses_rapid_repeats():
    clk = Clock(); g = ImuGestures(_now=clk)
    assert g.feed(imu(ax=0, ay=0, az=2.2)) == ["imu.tap"]
    clk.tick(0.1)
    assert g.feed(imu(ax=0, ay=0, az=2.2)) == []
    clk.tick(1.0)
    assert g.feed(imu(ax=0, ay=0, az=2.2)) == ["imu.tap"]


def test_trigger_catalogue_is_complete():
    for name in ("A.single", "A.double", "B.hold", "PWR.single", "imu.shake", "imu.tilt_left", "imu.tilt_toward"):
        assert name in ALL_TRIGGERS
    assert "PWR.down" not in ALL_TRIGGERS
