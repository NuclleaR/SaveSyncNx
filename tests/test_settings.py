"""The Koofr login: typed on the console, sealed in config/savesync.vault, tested at once."""

import socket

from conftest import APP_ROOT

EMAIL = {"kind": "Keyboard", "text": "me@example.com"}
PASSWORD = {"kind": "Keyboard", "text": "app-pass"}
OK = "Connected to Koofr as me@example.com."
WRONG = "Koofr rejected the email or app password (401).\nUse an app password from Koofr's settings,\nnot the account password."
DAMAGED = "The saved Koofr login is damaged.\nEnter it again in Settings."
FIRST_NAME = "Profile name (A-Z, 0-9, space, - or _)"
QUIT = {"kind": "Keyboard", "text": ""}  # cancel the first-name keyboard of the first dialog
SETTINGS_ITEMS = ["Change email and app password", "Test connection"]


def prompts(r):
    return [u["prompt"] for u in r.ui]


def vault(sd):
    return sd / APP_ROOT / "config" / "savesync.vault"


def test_first_run_opens_settings(run_script, dav, sd):
    r = run_script([EMAIL, PASSWORD, {"kind": "Message"}, QUIT], koofr_url=dav.url)
    assert r.exit == 0, r.output
    assert r.ui[0]["prompt"] == "Koofr email"
    assert r.ui[1]["prompt"] == "Koofr app password"
    assert OK in prompts(r)
    assert r.ui[-1]["prompt"] == FIRST_NAME
    blob = vault(sd).read_bytes()
    assert b"me@example.com" not in blob
    assert b"app-pass" not in blob


def test_wrong_password_explained(run_script, dav):
    r = run_script([EMAIL, {"kind": "Keyboard", "text": "wrong"}, {"kind": "Message"}, QUIT], koofr_url=dav.url)
    assert r.exit == 0, r.output
    assert WRONG in prompts(r)


def test_status_shows_email(run_script, dav, seed_vault):
    seed_vault()
    r = run_script([QUIT], koofr_url=dav.url)
    assert r.exit == 0, r.output
    assert "Koofr: me@example.com" in r.status
    assert r.ui[-1]["prompt"] == FIRST_NAME


def test_first_run_cancel_writes_nothing(run_script, dav, sd):
    r = run_script([{"kind": "Keyboard", "text": ""}], koofr_url=dav.url)
    assert r.exit == 0, r.output
    assert not vault(sd).exists()
    assert FIRST_NAME not in prompts(r)


def test_cancel_keeps_old_vault(run_unit, dav, sd, seed_vault):
    seed_vault()
    before = vault(sd).read_bytes()
    body = 'init_paths();\n    vault_read();\n    printf("rc=%d\\n", settings_change());\n    printf("user=%s\\n", g_user);'
    r = run_unit(body, koofr_url=dav.url, answers=[{"kind": "Keyboard", "text": "other@example.com"}, {"kind": "Keyboard", "text": ""}])
    assert r.exit == 0, r.output
    assert "rc=0" in r.output
    assert vault(sd).read_bytes() == before
    r = run_unit(body, koofr_url=dav.url, answers=[{"kind": "Keyboard", "text": ""}])
    assert "rc=0" in r.output
    assert vault(sd).read_bytes() == before
    r = run_unit('init_paths();\n    printf("read=%d user=%s\\n", vault_read(), g_user);', koofr_url=dav.url)
    assert "read=1 user=me@example.com" in r.output


def test_damaged_vault_goes_to_settings(run_script, run_unit, dav, sd):
    vault(sd).parent.mkdir(parents=True)
    vault(sd).write_bytes(b"not a sealed blob at all")
    r = run_script([{"kind": "Message"}, EMAIL, PASSWORD, {"kind": "Message"}, QUIT], koofr_url=dav.url)
    assert r.exit == 0, r.output
    assert r.ui[0]["prompt"] == DAMAGED
    assert r.ui[1]["prompt"] == "Koofr email"
    assert OK in prompts(r)
    r = run_unit('init_paths();\n    printf("read=%d user=%s\\n", vault_read(), g_user);', koofr_url=dav.url)
    assert "read=1 user=me@example.com" in r.output


def test_test_connection_menu(run_unit, dav, seed_vault):
    seed_vault()
    r = run_unit('init_paths();\n    vault_read();\n    settings_menu();\n    printf("done\\n");', koofr_url=dav.url,
                 answers=[{"kind": "PickOne", "item": "Test connection"}, {"kind": "Message"}])
    assert r.exit == 0, r.output
    assert r.ui[0]["prompt"] == "Settings"
    assert r.ui[0]["items"] == SETTINGS_ITEMS
    assert r.ui[1]["prompt"] == OK
    assert "done" in r.output


def test_change_from_menu_updates_vault_and_status(run_unit, dav, seed_vault):
    seed_vault(user="old@example.com")
    r = run_unit('init_paths();\n    vault_read();\n    settings_menu();\n    vault_read();\n    printf("user=%s\\n", g_user);', koofr_url=dav.url,
                 answers=[{"kind": "PickOne", "item": "Change email and app password"}, EMAIL, PASSWORD, {"kind": "Message"}])
    assert r.exit == 0, r.output
    assert "user=me@example.com" in r.output
    assert "Koofr: me@example.com" in r.status


def test_test_copy_404_and_transfer_error(run_unit, dav, seed_vault):
    seed_vault()
    r = run_unit('init_paths();\n    vault_read();\n    printf("t=%d\\n", dav_test());', koofr_url=dav.origin + "/nope",
                 answers=[{"kind": "Message"}])
    assert "t=0" in r.output, r.output
    assert r.ui[0]["prompt"] == "Koofr has no WebDAV folder at that address (404)."
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    r = run_unit('init_paths();\n    vault_read();\n    printf("t=%d fatal=%d\\n", dav_test(), g_fatal);', koofr_url=f"http://127.0.0.1:{port}/dav/Koofr",
                 answers=[{"kind": "Message"}])
    assert "t=0 fatal=0" in r.output, r.output
    assert r.ui[0]["prompt"].startswith("No connection to Koofr (transfer error -")
    assert r.ui[0]["prompt"].endswith(").\nCheck Wi-Fi.")
