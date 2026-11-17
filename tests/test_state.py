"""The state file: the fingerprint of each save's last upload."""

from conftest import APP_ROOT

KEY_A = "a:Serhii:0100ABCD00000000"
KEY_D = "d:0100ABCD00000000"


def state_file(sd):
    return sd / APP_ROOT / "config" / "savesync-state.txt"


def show(key):
    return f'    printf("{key}=%s\\n", state_get("{key}") == NULL ? "(none)" : state_get("{key}"));\n'


def test_set_get_across_runs(run_unit, sd):
    r = run_unit(f'init_paths();\n    state_load();\n    state_set("{KEY_A}", "1:2:abcd0123");\n    state_set("{KEY_D}", "3:4:00000001");\n'
                 f'    printf("set=%d\\n", state_set("{KEY_A}", "5:6:ffffffff"));')
    assert r.exit == 0, r.output
    assert "set=1" in r.output
    r = run_unit('init_paths();\n    printf("n=%d\\n", state_load());\n' + show(KEY_A) + show(KEY_D) + show("a:Son:0100ABCD00000000"))
    assert r.exit == 0, r.output
    assert "n=2" in r.output
    assert f"{KEY_A}=5:6:ffffffff" in r.output
    assert f"{KEY_D}=3:4:00000001" in r.output
    assert "a:Son:0100ABCD00000000=(none)" in r.output
    assert state_file(sd).read_text().count(KEY_A) == 1


def test_malformed_line_ignored(run_unit, sd):
    state_file(sd).parent.mkdir(parents=True)
    state_file(sd).write_text("good\tv1\nno-tab-line\n\tnokey\nnovalue\t\nk2\tv2\r\n")
    r = run_unit('init_paths();\n    printf("n=%d\\n", state_load());\n' + show("good") + show("k2") + show("no-tab-line") + show("novalue"))
    assert r.exit == 0, r.output
    assert "n=2" in r.output
    assert "good=v1" in r.output
    assert "k2=v2" in r.output
    assert "no-tab-line=(none)" in r.output
    assert "novalue=(none)" in r.output


def test_missing_file_empty(run_unit):
    r = run_unit('init_paths();\n    printf("n=%d\\n", state_load());\n' + show(KEY_A))
    assert r.exit == 0, r.output
    assert "n=0" in r.output
    assert f"{KEY_A}=(none)" in r.output


def test_set_is_atomic_rename(run_unit, sd):
    r = run_unit(f'init_paths();\n    state_load();\n    printf("set=%d\\n", state_set("{KEY_A}", "1:2:abcd0123"));')
    assert "set=1" in r.output, r.output
    assert state_file(sd).read_text() == f"{KEY_A}\t1:2:abcd0123\n"
    assert not (state_file(sd).parent / "savesync-state.txt.tmp").exists()


def test_set_on_fat_replaces_the_file(run_unit, sd):
    # FAT's rename refuses an existing file: the state file is removed first
    r = run_unit(f'init_paths();\n    state_load();\n    state_set("{KEY_A}", "1:2:abcd0123");\n'
                 f'    printf("set=%d\\n", state_set("{KEY_A}", "5:6:ffffffff"));', faults=["--fat-rename"])
    assert "set=1" in r.output, r.output
    r = run_unit('init_paths();\n    state_load();\n' + show(KEY_A))
    assert f"{KEY_A}=5:6:ffffffff" in r.output
    assert not (state_file(sd).parent / "savesync-state.txt.tmp").exists()


def test_leftover_tmp_is_recovered(run_unit, sd):
    tmp = state_file(sd).parent / "savesync-state.txt.tmp"
    tmp.parent.mkdir(parents=True)
    tmp.write_text(f"{KEY_A}\t1:2:abcd0123\n")
    r = run_unit('init_paths();\n    printf("n=%d\\n", state_load());\n' + show(KEY_A))
    assert r.exit == 0, r.output
    assert f"{KEY_A}=1:2:abcd0123" in r.output
