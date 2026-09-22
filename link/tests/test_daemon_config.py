import tomllib

from swlink.daemon import Hub, dump_config
from swlink.keymap import Action


def test_dump_config_round_trips_through_tomllib():
    cfg = {"device": {"port": "/dev/cu.usbmodem1"}, "daemon": {"port": 9898, "sync_rtc": True, "imu_hz": 20},
           "keys": {"A.click": "space", "B.hold": "cmd+shift+4", 'PWR.click': 'tap:esc'}}
    back = tomllib.loads(dump_config(cfg))
    assert back == cfg


def test_hub_set_keys_validates_and_persists(tmp_path):
    hub = Hub({"keys": {"A.click": "space"}}, None, tmp_path / "swlink.toml")
    assert hub.set_keys({"B.click": "enter", "PWR.hold": "cmd+q"}) is None
    assert hub.mapper.actions == {"B.click": Action("tap", ("enter",)), "PWR.hold": Action("tap", ("cmd", "q"))}
    saved = tomllib.loads((tmp_path / "swlink.toml").read_text())
    assert saved["keys"] == {"B.click": "enter", "PWR.hold": "cmd+q"}


def test_hub_set_keys_rejects_bad_action_without_changing_mapping(tmp_path):
    hub = Hub({"keys": {"A.single": "space"}}, None, tmp_path / "swlink.toml")
    assert "verb" in hub.set_keys({"A.single": "smash:space"})
    assert hub.mapper.actions == {"A.single": Action("tap", ("space",))}
    assert not (tmp_path / "swlink.toml").exists()


def test_legacy_click_mappings_migrate_to_single(tmp_path):
    hub = Hub({"keys": {"A.click": "space", "imu.shake": "esc"}}, None, tmp_path / "swlink.toml")
    assert set(hub.mapper.actions) == {"A.single", "imu.shake"}


def test_moods_round_trip_and_validation(tmp_path):
    import tomllib
    hub = Hub({"keys": {}}, None, tmp_path / "swlink.toml")
    assert hub.set_keys({"A.single": "space"}, {"A.single": "celebrate", "imu.shake": "error:800"}) is None
    saved = tomllib.loads((tmp_path / "swlink.toml").read_text())
    assert saved["moods"] == {"A.single": "celebrate", "imu.shake": "error:800"}
    assert "unknown mood" in hub.set_keys({}, {"B.single": "grumpy"})
