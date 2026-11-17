"""hostrun runs a script in Checkpoint's real interpreter with a fake console."""

HEADERS = "#include <checkpoint.h>\n#include <stdio.h>\n#include <stdlib.h>\n"


def test_runs_script_and_captures_output(run_raw):
    r = run_raw(HEADERS + 'int main(int argc, char** argv)\n{\n    printf("hi\\n");\n    return 0;\n}\n')
    assert r.exit == 0
    assert "hi" in r.output


def test_fat_rename_refuses_existing_destination(run_raw):
    # proves the FAT branches the --fat-rename tests rely on are really taken
    body = ('int main(int argc, char** argv)\n{\n'
            '    FILE* f = fopen("a.txt", "w");\n    fputs("a", f);\n    fclose(f);\n'
            '    f = fopen("b.txt", "w");\n    fputs("b", f);\n    fclose(f);\n'
            '    printf("taken=%d\\n", rename("a.txt", "b.txt"));\n'
            '    printf("free=%d\\n", rename("a.txt", "c.txt"));\n    return 0;\n}\n')
    r = run_raw(HEADERS + body, faults=["--fat-rename"])
    assert "taken=-1" in r.output and "free=0" in r.output, r.output
    r = run_raw(HEADERS + body)
    assert "taken=0" in r.output, r.output


def test_catalog_and_save_read(run_raw, catalog):
    catalog.add("0100ABCD00000000", "Some Game", "account", {"/a.bin": b"xyz"})
    r = run_raw(HEADERS + """
int main(int argc, char** argv)
{
    char* p;
    char* data;
    int size;
    int h;
    printf("count=%d\\n", titles_count());
    p = title_backup_path(0, 0);
    printf("path=%s\\n", p);
    free(p);
    h = sav_open(0, 0);
    sav_read(h, "/a.bin", &data, &size);
    printf("data=%s\\n", data);
    free(data);
    sav_close(h);
    return 0;
}
""")
    assert r.exit == 0, r.output
    assert "count=1" in r.output
    assert "path=sdmc:/switch/Checkpoint/saves/0x0100ABCD00000000 Some Game/" in r.output
    assert "data=xyz" in r.output


def test_dialog_answer_by_item(run_raw):
    r = run_raw(HEADERS + """
int main(int argc, char** argv)
{
    char* items[2];
    items[0] = "a";
    items[1] = "b";
    printf("picked %d\\n", gui_pick_one("Pick", items, 2));
    return 0;
}
""", answers=[{"kind": "PickOne", "item": "b"}])
    assert r.exit == 0, r.output
    assert r.ui[0]["kind"] == "PickOne"
    assert r.ui[0]["items"] == ["a", "b"]
    assert "picked 1" in r.output


def test_unanswered_dialog_is_runner_error(run_raw):
    r = run_raw(HEADERS + 'int main(int argc, char** argv)\n{\n    gui_message("Pick me");\n    return 0;\n}\n')
    assert r.exit == 3
    assert "Pick me" in r.output


def test_real_interpreter_rejects_const(run_raw):
    r = run_raw(HEADERS + "int main(int argc, char** argv)\n{\n    const int x = 1;\n    return 0;\n}\n")
    assert r.exit != 0


def test_unit_wrapper_includes_savesync(run_unit):
    r = run_unit('printf("ok %s\\n", SAVESYNC_VERSION);')
    assert r.exit == 0, r.output
    assert "ok v0.1.0" in r.output


def test_status_texts_recorded_in_order(run_raw):
    r = run_raw(HEADERS + 'int main(int argc, char** argv)\n{\n    gui_status("one");\n    gui_status("two");\n    return 0;\n}\n')
    assert r.exit == 0, r.output
    assert r.status == ["one", "two"]


def test_script_log_and_long_lines_reach_output(run_raw):
    long_line = "x" * 300
    r = run_raw(HEADERS + 'int main(int argc, char** argv)\n{\n    script_log("logged");\n    printf("%s\\n", "' + long_line + '");\n    return 0;\n}\n')
    assert r.exit == 0, r.output
    assert "logged\n" in r.output
    assert long_line in r.output


def test_save_writes_recorded(run_raw, catalog):
    catalog.add("0100ABCD00000000", "Some Game", "account", {"/a.bin": b"xyz"})
    r = run_raw(HEADERS + """
int main(int argc, char** argv)
{
    int h;
    h = sav_open(0, 0);
    sav_write(h, "/x", "1", 1);
    sav_close(h);
    return 0;
}
""")
    assert r.exit == 0, r.output
    assert r.save_writes == ["sav_write /x"]
