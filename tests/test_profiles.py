"""Profiles and the first dialog."""

import pytest

from conftest import APP_ROOT

KEYBOARD = "Profile name (A-Z, 0-9, space, - or _)"
BAD = "Use 1-24 Latin letters, digits, spaces, - or _."
DUPLICATE = "That name is already in the list."
SPACES = "The name must not start or end with a space."
TAIL = ["New name...", "Restore...", "Settings...", "Update script..."]


@pytest.fixture
def profiles(sd):
    path = sd / APP_ROOT / "config" / "savesync-profiles.txt"

    def write(*names):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("".join(n + "\n" for n in names))

    write.path = path
    return write


@pytest.fixture
def run(run_script, dav, seed_vault):
    seed_vault()
    return lambda answers: run_script(answers, koofr_url=dav.url)


def pick(item):
    return {"kind": "PickOne", "item": item}


def key(text):
    return {"kind": "Keyboard", "text": text}


MESSAGE = {"kind": "Message"}


def test_first_name_by_keyboard(run, profiles):
    r = run([key("Serhii"), MESSAGE])
    assert r.exit == 0, r.output
    assert r.ui[0]["prompt"] == KEYBOARD
    assert "backup as Serhii" in r.output
    assert profiles.path.read_text() == "Serhii\n"


def test_dialog_items(run, profiles):
    profiles("Serhii", "Son")
    r = run([pick("Son"), MESSAGE])
    assert r.exit == 0, r.output
    assert r.ui[0]["prompt"] == "SaveSync v0.1.0"
    assert r.ui[0]["items"] == ["Serhii", "Son"] + TAIL
    assert "backup as Son" in r.output


def test_bad_names_refused(run, profiles):
    r = run([key("Сергій"), MESSAGE, key("a/b"), MESSAGE, key("x" * 25), MESSAGE, key("Ok Name_1-2"), MESSAGE])
    assert r.exit == 0, r.output
    assert [u["prompt"] for u in r.ui][:7] == [KEYBOARD, BAD, KEYBOARD, BAD, KEYBOARD, BAD, KEYBOARD]
    assert "backup as Ok Name_1-2" in r.output
    assert profiles.path.read_text() == "Ok Name_1-2\n"


def test_name_check_rules(run_unit):
    r = run_unit(
        'struct names n;\n    n.count = 0;\n    n.cap = 0;\n    n.items = NULL;\n    names_add(&n, "Serhii");\n'
        '    printf("empty=%d\\n", profile_name_check("", &n));\n'
        '    printf("24=%d\\n", profile_name_check("abcdefghijklmnopqrstuvwx", &n));\n'
        '    printf("25=%d\\n", profile_name_check("abcdefghijklmnopqrstuvwxy", &n));\n'
        '    printf("dup=%d\\n", profile_name_check("SERHII", &n));\n'
        '    printf("dot=%d\\n", profile_name_check("a.b", &n));\n'
        '    printf("ok=%d\\n", profile_name_check("Son 2_x-y", &n));\n'
        '    printf("lead=%d\\n", profile_name_check(" Son", &n));\n'
        '    printf("trail=%d\\n", profile_name_check("Son ", &n));\n'
        '    printf("spaces=%d\\n", profile_name_check("   ", &n));\n'
        '    names_free(&n);'
    )
    assert r.exit == 0, r.output
    for line in ["empty=0", "24=1", "25=0", "dup=-1", "dot=0", "ok=1", "lead=-2", "trail=-2", "spaces=-2"]:
        assert line in r.output


def test_edge_spaces_refused(run, profiles):
    # the name goes into file names in Koofr, where an edge space is invisible
    r = run([key(" Serhii"), MESSAGE, key("Serhii "), MESSAGE, key("   "), MESSAGE, key("Serhii"), MESSAGE])
    assert r.exit == 0, r.output
    assert [u["prompt"] for u in r.ui][:7] == [KEYBOARD, SPACES, KEYBOARD, SPACES, KEYBOARD, SPACES, KEYBOARD]
    assert profiles.path.read_text() == "Serhii\n"


def test_duplicate_ignoring_case_refused(run, profiles):
    profiles("Serhii")
    r = run([pick("New name..."), key("serhii"), MESSAGE, key("Son"), MESSAGE])
    assert r.exit == 0, r.output
    assert r.ui[2]["prompt"] == DUPLICATE
    assert "backup as Son" in r.output
    assert profiles.path.read_text() == "Serhii\nSon\n"


def test_new_name_appended_in_order(run, profiles):
    profiles("Serhii", "Son")
    r = run([pick("New name..."), key("Guest"), MESSAGE])
    assert r.exit == 0, r.output
    assert "backup as Guest" in r.output
    assert profiles.path.read_text() == "Serhii\nSon\nGuest\n"


def test_new_name_cancel_returns_to_dialog(run, profiles):
    profiles("Serhii")
    r = run([pick("New name..."), key(""), pick("Serhii"), MESSAGE])
    assert r.exit == 0, r.output
    assert "backup as Serhii" in r.output
    assert profiles.path.read_text() == "Serhii\n"


def test_restore_and_settings_entries(run, profiles):
    profiles("Serhii")
    r = run([pick("Settings..."), pick("Test connection"), MESSAGE, pick("Restore..."), MESSAGE])
    assert r.exit == 0, r.output
    assert [u["prompt"] for u in r.ui] == [
        "SaveSync v0.1.0", "Settings", "Connected to Koofr as me@example.com.", "SaveSync v0.1.0",
        "There are no backups in Koofr for the games on this console.",
    ]
    assert "backup as" not in r.output


def test_update_entry_returns_to_dialog(run, profiles):
    profiles("Serhii")
    r = run([pick("Update script..."), MESSAGE, {"kind": "PickOne", "index": -1}])
    assert r.exit == 0, r.output
    assert [u["prompt"] for u in r.ui][0] == "SaveSync v0.1.0"
    assert r.ui[1]["prompt"].startswith("Could not update: GitHub could not be reached (")
    assert r.ui[2]["prompt"] == "SaveSync v0.1.0"


def test_cancel_first_dialog_quits(run, profiles):
    profiles("Serhii")
    r = run([{"kind": "PickOne", "index": -1}])
    assert r.exit == 0, r.output
    assert "backup as" not in r.output


def test_first_keyboard_cancel_quits(run, profiles):
    r = run([key("")])
    assert r.exit == 0, r.output
    assert "backup as" not in r.output
    assert not profiles.path.exists()


def test_unflushed_profiles_reported(run_unit):
    r = run_unit('init_paths();\n    struct names n;\n    names_init(&n);\n    names_add(&n, "Serhii");\n'
                 '    printf("saved=%d\\n", profiles_save(&n));',
                 faults=["--fail-close", "savesync-profiles.txt"])
    assert "saved=0" in r.output, r.output
