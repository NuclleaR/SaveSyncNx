"""Shared fixtures: run a Checkpoint script on the PC through build/hostrun.

hostrun runs Checkpoint 5.3.0's real picoc interpreter and script bindings with a
fake console: the catalog and the save archives come from folders on disk, the
dialogs are answered from a list, and the SD card is a temporary folder that
holds a directory literally named "sdmc:" so Checkpoint's "sdmc:/switch/..."
paths resolve inside it.
"""

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
HOSTRUN = ROOT / "build" / "hostrun"
SAVESYNC = ROOT / "scripts" / "savesync.c"
APP_ROOT = Path("sdmc:") / "switch" / "Checkpoint"


@dataclass
class Result:
    exit: int
    output: str
    ui: list = field(default_factory=list)
    status: list = field(default_factory=list)
    save_writes: list = field(default_factory=list)
    cancelled: bool = False


class Catalog:
    """Titles the fake console lists, each with a folder holding its save files."""

    def __init__(self, base: Path):
        self.base = base
        self.titles = []

    def add(self, title_id, name, kind="account", files=None, has_dir=True):
        save_dir = self.base / f"{kind}-{title_id}-{len(self.titles)}"
        entry = {"id": title_id, "name": name, "kind": kind}
        if has_dir:
            save_dir.mkdir(parents=True)
            for rel, data in (files or {}).items():
                target = save_dir / rel.lstrip("/")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            entry["dir"] = str(save_dir)
        self.titles.append(entry)
        return save_dir

    def write(self, path: Path):
        path.write_text(json.dumps({"titles": self.titles}))


@pytest.fixture
def sd(tmp_path):
    work = tmp_path / "sd"
    (work / APP_ROOT).mkdir(parents=True)
    return work


@pytest.fixture
def catalog(tmp_path):
    return Catalog(tmp_path / "saves-src")


@pytest.fixture
def dav(tmp_path):
    """A running fake Koofr (tests/fakedav.py) for me@example.com / app-pass."""
    from fakedav import FakeDav

    server = FakeDav(root=tmp_path / "dav", user="me@example.com", password="app-pass")
    yield server
    server.stop()


def _run(source, answers, sd_dir, cat, tmp):
    tmp.mkdir(parents=True, exist_ok=True)
    script = tmp / "script.c"
    script.write_text(source)
    answers_file = tmp / "answers.json"
    answers_file.write_text(json.dumps(answers or []))
    catalog_file = tmp / "catalog.json"
    cat.write(catalog_file)
    result_file = tmp / "result.json"
    if result_file.exists():
        result_file.unlink()
    proc = subprocess.run(
        [str(HOSTRUN), "--catalog", str(catalog_file), "--answers", str(answers_file),
         "--result", str(result_file), str(script)],
        cwd=sd_dir, capture_output=True, text=True, timeout=300,
    )
    if not result_file.exists():
        return Result(exit=proc.returncode, output=proc.stdout + proc.stderr)
    data = json.loads(result_file.read_text())
    return Result(
        exit=data["exit"],
        output=data["output"] + proc.stderr,
        ui=data["ui"],
        status=data["status"],
        save_writes=data["saveWrites"],
        cancelled=data["cancelled"],
    )


def _defines(koofr_url, defines):
    lines = []
    if koofr_url is not None:
        lines.append(f'#define KOOFR_URL "{koofr_url}"')
    for name, value in (defines or {}).items():
        lines.append(f'#define {name} "{value}"')
    return "\n".join(lines) + "\n"


@pytest.fixture
def run_raw(sd, catalog, tmp_path):
    """Run an arbitrary script source."""
    counter = {"n": 0}

    def run(source, answers=None):
        counter["n"] += 1
        return _run(source, answers, sd, catalog, tmp_path / f"run{counter['n']}")

    return run


@pytest.fixture
def run_unit(run_raw):
    """Run `body` as main() of a script that includes savesync.c without its main."""

    def run(body, prelude="", koofr_url=None, answers=None, defines=None):
        source = (
            _defines(koofr_url, defines)
            + "#define SAVESYNC_NO_MAIN 1\n"
            + f'#include "{SAVESYNC}"\n'
            + prelude
            + "\nint main(int argc, char** argv)\n{\n"
            + body
            + "\n    return 0;\n}\n"
        )
        return run_raw(source, answers)

    return run


@pytest.fixture
def seed_vault(run_unit):
    """Write config/savesync.vault for `user`/`pw` with the script's own vault_write()."""

    def seed(user="me@example.com", pw="app-pass"):
        r = run_unit(f'init_paths();\n    strcpy(g_user, "{user}");\n    strcpy(g_pw, "{pw}");\n    printf("vault=%d\\n", vault_write());')
        assert "vault=1" in r.output, r.output

    return seed


@pytest.fixture
def run_script(run_raw):
    """Run savesync.c itself (its own main) against the given server URL."""

    def run(answers, koofr_url, defines=None):
        source = _defines(koofr_url, defines) + f'#include "{SAVESYNC}"\n'
        return run_raw(source, answers)

    return run
