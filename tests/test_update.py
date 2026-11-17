"""Update: "Update script..." replaces the installed script with the latest GitHub release."""

import json

import pytest

from conftest import APP_ROOT

API = "/gh/api/releases/latest"
RAW = "/gh/raw"
CHECKING = "Checking GitHub for a new version..."
LATEST = "SaveSync v0.1.0 is the latest version."
OFFER = "Update SaveSync from v0.1.0 to v0.2.0?\n\nThe new version runs from the next start."
DONE = "Updated to v0.2.0. Run savesync again to use it."
NEW = b'/* SaveSync */\n#define SAVESYNC_VERSION "v0.2.0"\nint main(void) { return 0; }\n'


def failed(reason):
    return f"Could not update: {reason}.\nNothing was changed."


def pick(item):
    return {"kind": "PickOne", "item": item}


BACK = {"kind": "PickOne", "index": -1}
YES = {"kind": "Confirm", "yes": True}
NO = {"kind": "Confirm", "yes": False}
MESSAGE = {"kind": "Message"}


@pytest.fixture
def installed(sd):
    path = sd / APP_ROOT / "scripts" / "universal" / "savesync.c"
    path.parent.mkdir(parents=True)
    path.write_bytes(b"installed v0.1.0\n")
    return path


@pytest.fixture
def update(run_script, dav, seed_vault, sd):
    seed_vault()
    (sd / APP_ROOT / "config" / "savesync-profiles.txt").write_text("Serhii\n")
    defines = {"GITHUB_API": dav.origin + API, "GITHUB_RAW": dav.origin + RAW}

    def run(answers, faults=None):
        r = run_script([pick("Update script...")] + answers + [BACK], koofr_url=dav.url, defines=defines, faults=faults)
        assert r.exit == 0, r.output
        assert r.save_writes == []
        return r

    return run


def release(dav, tag="v0.2.0", body=NEW, api_status=200, raw_status=200):
    dav.static[API] = (api_status, json.dumps({"tag_name": tag, "name": tag}).encode() if api_status == 200 else b"")
    dav.static[f"{RAW}/{tag}/scripts/savesync.c"] = (raw_status, body if raw_status == 200 else b"")


def no_leftovers(sd):
    work = sd / APP_ROOT / "savesync"
    return not work.exists() or list(work.iterdir()) == []


def test_latest_version_says_so(update, dav, installed):
    release(dav, tag="v0.1.0")
    r = update([MESSAGE])
    assert r.ui[1]["prompt"] == LATEST
    assert CHECKING in r.status
    assert r.ui[-1]["prompt"] == "SaveSync v0.1.0"
    assert installed.read_bytes() == b"installed v0.1.0\n"


def test_offer_shows_both_versions(update, dav, installed):
    release(dav)
    r = update([NO])
    assert r.ui[1]["kind"] == "Confirm"
    assert r.ui[1]["prompt"] == OFFER


def test_confirm_replaces_script(update, dav, installed, sd):
    release(dav)
    r = update([YES, MESSAGE])
    assert r.ui[2]["prompt"] == DONE
    assert installed.read_bytes() == NEW
    assert no_leftovers(sd)
    assert r.ui[-1]["prompt"] == "SaveSync v0.1.0"


def test_confirm_installs_when_folder_missing(update, dav, sd):
    release(dav)
    r = update([YES, MESSAGE])
    assert r.ui[2]["prompt"] == DONE
    assert (sd / APP_ROOT / "scripts" / "universal" / "savesync.c").read_bytes() == NEW


def test_unflushed_download_keeps_installed(update, dav, installed, sd):
    # the SD card fails the final flush of the downloaded script
    release(dav)
    r = update([YES, MESSAGE], faults=["--fail-close", "savesync/update.c"])
    assert r.ui[2]["prompt"] == failed("the script could not be written to the SD card")
    assert installed.read_bytes() == b"installed v0.1.0\n"
    assert no_leftovers(sd)


def test_cancel_changes_nothing(update, dav, installed, sd):
    release(dav)
    update([NO])
    assert installed.read_bytes() == b"installed v0.1.0\n"
    assert [p for m, p, _ in dav.request_headers if p.startswith(RAW)] == []
    assert no_leftovers(sd)


def test_wrong_payload_rejected(update, dav, installed, sd):
    release(dav, body=b'#define SAVESYNC_VERSION "v0.1.9"\nnot it\n')
    r = update([YES, MESSAGE])
    assert r.ui[2]["prompt"] == failed("the download is not SaveSync v0.2.0")
    assert installed.read_bytes() == b"installed v0.1.0\n"
    assert no_leftovers(sd)


def test_github_unreachable(update, dav, installed):
    release(dav, api_status=500)
    r = update([MESSAGE])
    assert r.ui[1]["prompt"] == failed("GitHub could not be reached (500)")
    assert installed.read_bytes() == b"installed v0.1.0\n"


def test_no_release_yet(update, dav, installed):
    r = update([MESSAGE])
    assert r.ui[1]["prompt"] == failed("GitHub could not be reached (404)")


@pytest.mark.parametrize("answer", [b'{"name": "v0.2.0"}', b'{"tag_name": "../../evil"}', b"not json"])
def test_release_without_usable_tag(update, dav, installed, answer):
    dav.static[API] = (200, answer)
    r = update([MESSAGE])
    assert r.ui[1]["prompt"] == failed("GitHub's answer names no release")
    assert installed.read_bytes() == b"installed v0.1.0\n"


def test_download_unreachable(update, dav, installed, sd):
    release(dav, raw_status=404)
    r = update([YES, MESSAGE])
    assert r.ui[2]["prompt"] == failed("GitHub could not be reached (404)")
    assert installed.read_bytes() == b"installed v0.1.0\n"
    assert no_leftovers(sd)


def test_user_agent_sent(update, dav, installed):
    release(dav)
    update([YES, MESSAGE])
    gets = [(p, h) for m, p, h in dav.request_headers if m == "GET" and p.startswith("/gh/")]
    assert [p for p, _ in gets] == [API, f"{RAW}/v0.2.0/scripts/savesync.c"]
    assert all(h.get("User-Agent") == "savesync" for _, h in gets)
    assert all("Authorization" not in h for _, h in gets)
