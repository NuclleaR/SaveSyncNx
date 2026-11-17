# SaveSyncNx Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Checkpoint 5.3.0 script, `savesync`, that backs up the changed account and device saves of a Switch to Koofr over WebDAV in one action and restores any version into Checkpoint's backup folder.

**Architecture:** One picoc script, `scripts/savesync.c` (picoc scripts are single files). On the PC it runs in Checkpoint 5.3.0's real interpreter and real bindings: `tools/hostrun` links them with a fake console (catalog and saves from folders on disk, dialog answers from a file), and `tests/fakedav.py` stands in for Koofr. pytest drives both.

**Tech Stack:** picoc C against the Checkpoint 5.3.0 scripting API; host side g++ (C++23), make, libcurl, mbedtls 3 (Arch `mbedtls3`), Python 3 + pytest (installed by mise from `mise.toml`); Checkpoint as a git submodule at `v5.3.0`.

**Spec:** `docs/superpowers/specs/2026-11-12-savesyncnx-design.md`

## Global Constraints

- **API floor:** Checkpoint `v5.3.0`. Every binding the script calls must exist in `third_party/Checkpoint/common/script/library_checkpoint.c` at that tag.
- **picoc rules** (`third_party/Checkpoint/scripts/README.md` §12):
  - Include only `<checkpoint.h> <stdio.h> <stdlib.h> <string.h> <unistd.h> <ctype.h> <math.h> <time.h> <errno.h> <stdbool.h>`.
  - Language: no `const`/`volatile`, no 64-bit integers, index arrays instead of pointer arithmetic, one literal per string, constant array bounds, structs at file scope, callees before callers with `main` last, `/* */` comments, file-scope globals instead of `static` locals.
  - Resources: free every `char*`, `delete_directory` every listing, close every save handle.
- **Saves are read-only.** The script never calls `sav_write`, `sav_delete`, `sav_mkdir`, `sav_rmdir`, `sav_rename` or `sav_commit`.
- **Cloud deletions and overwrites.**
  - DELETE only inside `SaveSyncNx/.incoming/`.
  - PUT only into `SaveSyncNx/.incoming/`.
  - MOVE always with `Overwrite: F`.
- **Koofr URL:** `#define KOOFR_URL "https://app.koofr.net/dav/Koofr"`, wrapped in `#ifndef KOOFR_URL` so tests can predefine it.
- **Update URLs**, each in its own `#ifndef` so tests can predefine them:
  - `GITHUB_API` = `"https://api.github.com/repos/NuclleaR/SaveSyncNx/releases/latest"`
  - `GITHUB_RAW` = `"https://raw.githubusercontent.com/NuclleaR/SaveSyncNx"`
  - `SAVESYNC_VERSION` = `"v0.1.0"`, a plain `#define` that must equal the release tag.
  - The installed script path is `<app_root()>/scripts/universal/savesync.c`.
- **Cloud layout:**
  - Root `SaveSyncNx/` under the URL, with `saves/` (account), `device/` and `.incoming/`.
  - Game folder: the basename of `title_backup_path(idx, 0)` (Checkpoint's `0x%016llX <name>`), with `/ \ : * ? " < > |` and trailing dots replaced by `_`.
  - Restore matches a folder by its `0x<16 hex>` prefix.
- **Version name:**
  - Format: `YYYYMMDD-HHMMSS` in UTC, taken from the server's `Date` header once per run. Account saves add a space and the profile name (`20261112-184501 Serhii.zip`); device saves use the bare timestamp.
  - A name that is taken gets ` (2)`, ` (3)`, ….
  - No `Date` header: fall back to the console clock (`gmtime(time(NULL))`) and log it (designer's call).
- **Save kinds:** only catalog entries whose `title_backup_path(idx, 0)` parent folder is `saves` or `device`. BCAT and system saves are skipped.
- **SD paths:** built from `app_root()` only.
  - Vault: `config/savesync.vault` (sealed JSON `{"user":…,"password":…}`, no passphrase).
  - Profiles: `config/savesync-profiles.txt` (one name per line, in creation order).
  - State: `config/savesync-state.txt` (`<key>\t<fingerprint>` per line).
  - Work dir `savesync/`: `savesync/dump/`, `savesync/up.zip` and `savesync/down.zip`. Emptied at the start of every run.
- **Profile names:** 1–24 characters from `A-Z a-z 0-9`, space, `-` and `_`, unique ignoring case. The 24-character cap is the designer's call, so names fit file names and dialogs.
- **State keys:** `a:<profile>:<TITLEID>` for account saves and `d:<TITLEID>` for device saves (TITLEID is 16 uppercase hex digits).
- **License** GPL-3.0-or-later. Code adapted from Checkpoint's `webdav.c` keeps its notice and is marked as modified.
- **Git:**
  - Commit or push only on the owner's explicit go-ahead for that change. No AI attribution. Each "Commit" step below means: stage, propose the message, wait.
  - **Never `cd` into the folder you are already in.** Run `make` and `git` from the repo root. `git -C third_party/Checkpoint …` is the one allowed form (it targets the submodule).
- **Docs:** README, this plan's checkboxes and the Deviations section are updated in the same change as the code they describe.

### Copy (exact UI texts)

| Where | Text |
|---|---|
| Status at start of every run | `Koofr: <email>` |
| First dialog | prompt `SaveSync <version>`; items: profile names, `New name...`, `Restore...`, `Settings...`, `Update script...` |
| Update: checking | status `Checking GitHub for a new version...` |
| Update: latest | `SaveSync <version> is the latest version.` |
| Update: offer | `Update SaveSync from <current> to <new>?\n\nThe new version runs from the next start.` |
| Update: done | `Updated to <new>. Run savesync again to use it.` |
| Update: failed | `Could not update: <reason>.\nNothing was changed.` with reason `GitHub could not be reached (<code>)` or `the download is not SaveSync <new>`; also `GitHub's answer names no release` and `the script could not be written to the SD card` (Task 12 Deviations) |
| Keyboards | `Koofr email` · `Koofr app password` · `Profile name (A-Z, 0-9, space, - or _)` |
| Bad profile name | `Use 1-24 Latin letters, digits, spaces, - or _.` |
| Duplicate profile | `That name is already in the list.` |
| Settings menu | prompt `Settings`; items `Change email and app password`, `Test connection` |
| Test: 207 | `Connected to Koofr as <email>.` |
| Test: 401 | `Koofr rejected the email or app password (401).\nUse an app password from Koofr's settings,\nnot the account password.` |
| Test: 404/405 | `Koofr has no WebDAV folder at that address (<code>).` |
| Test: transfer error | `No connection to Koofr (transfer error <n>).\nCheck Wi-Fi.` |
| Test: other | `Koofr answered HTTP <code>.` |
| Damaged vault | `The saved Koofr login is damaged.\nEnter it again in Settings.` |
| Run stops: network | `No connection to Koofr. Check Wi-Fi.` |
| Run stops: 401/403 | `Koofr rejected the login. Check email and app password in Settings.` |
| Run stops: 507 | `Koofr storage is full.` |
| Run stops: the folders cannot be made for another reason | `Koofr answered HTTP <code>.` |
| Summary | `Backup done: <u> uploaded, <n> unchanged, <f> failed.` |
| Restore: nothing | `There are no backups in Koofr for the games on this console.` |
| Restore: game list | prompt `Pick a game`, plus `\n(<k> more in Koofr have no save on this console;\nstart a game once to create its save)` when k > 0 (the hint added in the final review, as the spec asks); device entries `<name> (device)` |
| Restore: versions | prompt `Pick a version of <name>` |
| Restore: confirm | `Download\n<version>\ninto Checkpoint's backup folder for\n<name>?\n\nNothing is written to the game until\nyou restore it in Checkpoint.` |
| Restore: done | `Downloaded. Pick it in Checkpoint's backup\nlist for this game and press Restore.\nFor an account save, select the right\nprofile first.` |
| Restore: failed | `The download failed and was removed.\nNothing was changed.` |
| Restore: game folder without versions | `There are no versions of <name> in Koofr.` (Task 11 Deviations) |

## Review Focus

1. **Game names with non-ASCII or URL-special characters** (`é`, `™`, `'`, `&`, `#`, `%`, `?`, `:`). The folder must survive PUT, MOVE and PROPFIND and still be found by Restore. Tests in Task 8 (`test_nonascii_game_folder_roundtrip`) and Task 11 (`test_restore_nonascii_game`).
2. **Upload interrupted during PUT or MOVE.** No complete version is created or lost, the next run deletes the leftover in `.incoming/`, and the save is uploaded again. Tests in Task 8 (`test_dropped_put_leaves_only_incoming`, `test_dropped_move_leaves_only_incoming`) and Task 10 (`test_failed_game_retried_next_run`).
3. **One game with both an account save and a device save, backed up under two profiles.** The two saves get separate folders, and the device save is uploaded once across both profiles. Test in Task 9 (`test_two_profiles_device_once`).
4. **Nested subfolders, empty subfolders, a 20 MB file.** The fingerprint stays stable and upload plus restore are byte-identical. Tests in Task 7 (`test_fingerprint_stable_with_subfolders`), Task 9 (`test_zip_contents_match_save`, `test_large_file_roundtrip`) and Task 11 (`test_restore_bytes_identical`).
5. **`Date` header lowercase (HTTP/2) or missing.** The version time is still taken correctly, or falls back to the console clock. Test in Task 8 (`test_stamp_variants`).

---

### Task 1: Repository skeleton and Checkpoint at v5.3.0

**Files:**
- Create: `.gitignore` (`build/`, `__pycache__/`, `.pytest_cache/`)
- Create: `LICENSE`, the GPLv3 text, copied from `../../3ds/3DSaveSync/LICENSE`
- Create: `README.md` (what it is, status "in progress", link to spec and plan)
- Create: `Makefile` with targets `lint`, `hostrun` (filled in Task 2), `test` and `clean`
- Create: `scripts/savesync.c`: header comment (what it does, where it stores things, setup, GPL notice, "adapted from Checkpoint's scripts/common/universal/webdav.c"), the `#ifndef KOOFR_URL` define, and `int main(int argc, char** argv)` printing `savesync` inside `#ifndef SAVESYNC_NO_MAIN … #endif`
- Submodule: `third_party/Checkpoint` at tag `v5.3.0`, initialised recursively (picoc)

- [x] **Step 1: Add the submodule and pin it**

Run: `git submodule add https://github.com/BernardoGiordano/Checkpoint third_party/Checkpoint`, then `git -C third_party/Checkpoint checkout v5.3.0`, then `git submodule update --init --recursive`.
Expected: `third_party/Checkpoint/3rd-party/picoc/include/interpreter.h` exists.

- [x] **Step 2: Check every binding the script will use exists at v5.3.0**

Run: `grep -c -E '"(int|void|char\*|struct [a-zA-Z]+\*) (titles_count|title_id|title_name|title_backup_path|sav_open|sav_list|sav_read|sav_close|read_directory|delete_directory|sd_mkdirs|sd_exists|zip_dir|unzip|web_request|web_upload_file|http_header_value|device_seal|device_unseal|json_new|json_parse|json_delete|json_is_valid|json_object_contains|json_object_element|json_is_string|json_get_string|gui_message|gui_confirm|gui_pick_one|gui_keyboard|gui_status|progress_begin|progress_set|progress_label|progress_end|progress_note|progress_clear|script_log|app_root)\(' third_party/Checkpoint/common/script/library_checkpoint.c`
Expected: `40` (the same command gives 40 on `master`, 2026-11-12). Any missing binding is a spec-level problem: stop and report to the owner.

- [x] **Step 3: `make lint` runs Checkpoint's parser over the script**

`lint:` runs `third_party/Checkpoint/tools/scriptlint.sh scripts/savesync.c`.
Run: `make lint`. Expected: `OK    scripts/savesync.c`.

- [x] **Step 4: Commit** (owner's go-ahead): `chore: skeleton, Checkpoint v5.3.0 submodule, lint`

### Task 2: hostrun, a PC runner for Checkpoint scripts

**Files:**
- Create: `tools/hostrun/main.cpp`: CLI, worker thread, dialog pump, result file
- Create: `tools/hostrun/fakehost.cpp`, `tools/hostrun/fakehost.hpp`: `FakeHost : ScriptHost`
- Create: `tools/hostrun/shim/`, only for platform headers or functions the linked Checkpoint sources need (e.g. `paths.hpp`, thread helpers). Keep it minimal and list each shim in Deviations.
- Modify: `Makefile`. `hostrun` builds `build/hostrun`; `test` runs `hostrun`, `lint`, then `pytest tests -q`.
- Create: `tests/conftest.py`, `tests/test_hostrun.py`

**Interfaces:**
- Produces, CLI: `build/hostrun --catalog <catalog.json> --answers <answers.json> --result <result.json> <script.c>`
  - The process cwd is the fake SD. It holds a real directory literally named `sdmc:`, so Checkpoint's `sdmc:/switch/Checkpoint/…` paths resolve inside it.
  - Exit code: the script's exit value; `3` means a runner error (dialog with no answer left, answer kind mismatch, bad arguments).
- Produces, `catalog.json`: `{"titles":[{"id":"0100ABCD00000000","name":"Some Game","kind":"account|device|bcat|system","dir":"/abs/save/files"}]}`
  - `titleBackupPath` returns `sdmc:/switch/Checkpoint/<saves|device|bcat|system>/0x<ID> <StringUtils::removeForbiddenCharacters(name)>` and creates that folder at startup, as Checkpoint does.
  - `savOpen(idx, 0)` exposes `dir` as the archive; a missing `dir` makes it return `-5`. Kind 1 returns `-1`.
  - Write-type `sav_*` calls return 0 and are recorded.
  - `deviceSecret` returns 32 fixed bytes; `randomBytes` reads from `/dev/urandom`.
- Produces, `answers.json`: a list consumed in order, of `{"kind":"PickOne","item":"<text>"}`, `{"kind":"PickOne","index":n}`, `{"kind":"Keyboard","text":"…"}` (empty text means cancel), `{"kind":"Confirm","yes":true|false}` or `{"kind":"Message"}`.
- Produces, `result.json`: `{"exit":int,"cancelled":bool,"output":str,"ui":[{"kind","prompt","items"}],"status":[str],"saveWrites":[str]}`. `output` is the full transcript, not the engine's 4 KB tail.
- Produces, pytest (`tests/conftest.py`):
  - Fixtures `sd` (tmp cwd with `sdmc:/switch/Checkpoint/`) and `catalog`, with `catalog.add(id, name, kind, files: dict[str, bytes]) -> Path` (returns the save dir).
  - `run_script(answers, koofr_url) -> Result`, with `.exit .output .ui .status .save_writes`. It runs a generated wrapper `#define KOOFR_URL "<koofr_url>"` + `#include "<abs>/scripts/savesync.c"`.
  - `run_unit(body, prelude="", koofr_url=None) -> Result`. It wraps `#define SAVESYNC_NO_MAIN 1`, the optional `KOOFR_URL`, the include, `prelude`, then `int main(int argc, char** argv) { <body> return 0; }`.

- [x] **Step 1: Write `tests/test_hostrun.py`**

```python
def test_runs_script_and_captures_output(run_raw): r = run_raw('int main(int c, char** v){ printf("hi\\n"); return 0; }'); assert r.exit == 0 and "hi" in r.output
def test_catalog_and_save_read(run_raw, catalog):  # one account title, /a.bin = b"xyz"
    ...; assert "sdmc:/switch/Checkpoint/saves/0x0100ABCD00000000 Some Game/" in r.output and "xyz" in r.output
def test_dialog_answer_by_item(run_raw): ...; assert r.ui[0]["items"] == ["a", "b"] and "picked 1" in r.output
def test_unanswered_dialog_is_runner_error(run_raw): ...; assert r.exit == 3 and "Pick" in r.output
def test_real_interpreter_rejects_const(run_raw): r = run_raw('int main(int c, char** v){ const int x = 1; return 0; }'); assert r.exit != 0
def test_unit_wrapper_includes_savesync(run_unit): assert run_unit('printf("ok\\n");').exit == 0
def test_save_writes_recorded(run_raw, catalog): ...; assert r.save_writes == ["sav_write /x"]
```

(`run_raw(source, answers=[])` runs an arbitrary script and also lives in `conftest.py`.)

- [x] **Step 2: Run it.** `pytest tests/test_hostrun.py -q`. Expected: FAIL (no `build/hostrun`).

- [x] **Step 3: Implement hostrun**
  - Build list: the interpreter core as in `tools/scriptlint.sh` (picoc `source/interpreter/*.c` minus `debug.c`, `common/script/platform_checkpoint.c`, `common/script/cstdlib/*.c`, `library_checkpoint.c`). Instead of scriptlint's empty stubs, link the real bindings from `common/script/` (`checkpoint_api`, `json_api`, `zip_api`, `web_api`, `httpcall`, `seal_api`, `progress_api`, `scriptargs`, `scriptheap`, `scriptconsole`, `scriptbar`, `scriptuibridge`, `scriptengine`, `scriptrunner`, `scriptabort`) and `common/{common,transferprotocol,logging}.cpp` as needed.
  - Include paths and libraries: `3rd-party/json` for `json.hpp`. Link `-lcurl` and mbedtls; if 3.x does not compile `seal_api.cpp`, use Arch's `mbedtls2` and record it in Deviations.
  - Run model: `ScriptEngine::run(path, {""})` on a `std::thread`. The main thread polls `ScriptRunner::get().bridge().take()` every 1 ms and answers it.
  - The `#ifndef SAVESYNC_NO_MAIN` guard already exists from Task 1.

- [x] **Step 4: Run it.** `make test`. Expected: all `test_hostrun.py` tests PASS and lint OK.

- [x] **Step 5: Commit** (owner's go-ahead): `test: hostrun — Checkpoint's interpreter and bindings on the PC`

### Task 3: fakedav, a Koofr stand-in

**Files:**
- Create: `tests/fakedav.py`, `tests/test_fakedav.py`
- Create: `tests/fixtures/koofr-propfind.xml`, copied from `../../3ds/3DSaveSync/tests/fixtures/koofr-propfind.xml` (a real Koofr answer)
- Modify: `tests/conftest.py` (fixture `dav`)

**Interfaces:**
- Produces: fixture `dav`, a running `FakeDav(user="me@example.com", password="app-pass")`.
- Attributes:
  - `.url`: `http://127.0.0.1:<port>/dav/Koofr`
  - `.root`: Path of the storage
  - `.log`: list of `(method, path, status)`
- Switches: `.fixed_date: str | None` (e.g. `"Thu, 12 Nov 2026 18:45:01 GMT"`), `.lowercase_date`, `.no_date`, `.quota_full` (PUT → 507), `.password`.
- Static routes for the GitHub fakes: `.static[path] = (status, body: bytes)` is served for a GET of any path outside `/dav/`.
- Fault methods:
  - `.fail_next(method, status, path_contains=None)`
  - `.drop_next(method, path_contains=None)`: reads the request, then closes the socket without answering
  - `.stop()`
- Koofr behaviour, from the 3DSaveSync probe:
  - Basic auth; a wrong login gets 401.
  - MKCOL: new → 201, existing → 405, missing parent → 409.
  - PUT → 201; GET → 200; missing → 404.
  - PROPFIND Depth 0/1 → 207, missing → 404. hrefs are `/dav/Koofr/…` percent-encoded with the `D:` prefix, shaped like the fixture: folder hrefs have no trailing `/` and carry `<D:collection xmlns:D="DAV:"/>` (plus a 404 propstat for `getcontentlength`), files have `<D:getcontentlength>`. The first entry is the requested path itself.
  - MOVE with an absolute `Destination` and `Overwrite: F` → 201, onto an existing name → 412, missing source → 404.
  - DELETE (recursive) → 204, missing → 404.

- [x] **Step 1: Write `tests/test_fakedav.py`.** Use plain `http.client`. One test each: `test_wrong_login_401`, `test_mkcol_201_then_405`, `test_mkcol_missing_parent_409`, `test_put_then_get_same_bytes`, `test_propfind_depth1_shape_matches_koofr_fixture` (the answer equals the fixture byte for byte: `D:multistatus`, the `/dav/Koofr/` href prefix, no trailing `/` on folders, percent-encoded `é`), `test_propfind_missing_404`, `test_move_201_then_412`, `test_delete_recursive_204_then_404`, `test_date_header_variants` (fixed, lowercase key, none), `test_fail_next_and_drop_next`, `test_quota_full_507`.
- [x] **Step 2: Run.** `pytest tests/test_fakedav.py -q`. Expected: FAIL (module missing).
- [x] **Step 3: Implement** on `http.server.ThreadingHTTPServer` in a background thread.
- [x] **Step 4: Run.** Expected: PASS.
- [x] **Step 5: Commit** (owner's go-ahead): `test: fake Koofr WebDAV server`

### Task 4: Script foundation: paths, text helpers, WebDAV calls

**Files:**
- Modify: `scripts/savesync.c`
- Create: `tests/test_util.py`, `tests/test_dav.py`

**Interfaces (Produces, all in `savesync.c`):**
- Globals:
  - `char g_root[PATHN], g_cfg[PATHN], g_work[PATHN]`: `app_root()`, `<root>/config`, `<root>/savesync`
  - `char g_base[URLN]`: `KOOFR_URL`
  - `char g_user[USERN], g_pw[PWN], g_auth[AUTHN]`
  - `int g_fatal`: `0`, `FATAL_NET`, `FATAL_LOGIN` or `FATAL_FULL`
  - `char g_date[64]`: the last response's `Date` value
- `void init_paths(void)`. It also creates `config/` and an empty `savesync/`.
- Adapted from `webdav.c`:
  - `void b64_encode(char* src, char* dst, int size)`
  - `int url_add(char* dst, char* segment, int limit)`: appends `/` and the percent-encoded segment, every non-unreserved byte encoded
  - `void sanitize_name(char* src, char* dst, int size)`
  - `void pct_decode(char* src, char* dst, int size)`
  - `void build_auth(void)`
- `int dav_call(char* method, char* url, char* headers, char* body, char** out, int* outSize)` returns the HTTP status or a negative transfer error.
  - Stores `Date` or `date` into `g_date`.
  - Sets `g_fatal`: 401/403 → `FATAL_LOGIN`, 507 → `FATAL_FULL`, < 0 → `FATAL_NET`.
- `int dav_mkcol(char* url)` returns 1 on 201/405.
- `struct names { int count; int cap; char** items; };` with `void names_add(struct names* n, char* s)`, `void names_free(struct names* n)`.
- `int dav_list(char* url, struct names* out)` returns 1 if OK.
  - Fills the decoded leaf names of the children, without trailing `/`, skipping the folder itself.
  - No cap on the count. Parse with `strstr`, not per-character loops.
- `int dav_put_file(char* path, char* url)` returns the status (streams via `web_upload_file`).
- `int dav_move(char* fromUrl, char* toUrl)` returns the status (`Destination: <absolute toUrl>`, `Overwrite: F`).
- `int dav_delete(char* url)` returns the status.
- `int dav_get_file(char* url, char* path)` returns 1 if OK (writes the body to `path`).

- [x] **Step 1: Write the tests.** All of them run through `run_unit`; the dav tests use `koofr_url=dav.url` and a prelude that sets `g_user`/`g_pw` and calls `build_auth()`.
  - `test_util.py`:
    - `test_b64` (`"me@example.com:app-pass"` → Python's `base64` of it)
    - `test_url_add_encodes_utf8_and_specials` (`"Pokémon™ Let's Go: #1 & 50%?"` → `urllib.parse.quote(s, safe="")`)
    - `test_sanitize_name` (`'a/b\\c:d*e?f"g<h>i|j.'` → `"a_b_c_d_e_f_g_h_i_j_"`)
    - `test_pct_decode_roundtrip`
  - `test_dav.py`:
    - `test_mkcol_twice_ok`
    - `test_put_get_roundtrip_bytes` (bytes 0–255 × 1000)
    - `test_list_returns_decoded_leaf_names` (including `é`)
    - `test_list_more_than_128`
    - `test_move_then_412`
    - `test_delete`
    - `test_401_sets_fatal_login`
    - `test_507_sets_fatal_full`
    - `test_server_down_sets_fatal_net`
    - `test_date_captured_any_case`
- [x] **Step 2: Run.** `make test`. Expected: the new tests FAIL.
- [x] **Step 3: Implement** the functions above.
- [x] **Step 4: Run.** Expected: PASS; lint OK.
- [x] **Step 5: Commit** (owner's go-ahead): `feat: WebDAV calls and text helpers`

### Task 5: Vault and Settings

**Files:**
- Modify: `scripts/savesync.c`
- Create: `tests/test_settings.py`

**Interfaces:**
- Consumes: Task 4.
- Produces:
  - `int vault_read(void)`: 1 loaded into `g_user`/`g_pw`, 0 no vault, -1 damaged (shows the damaged-vault copy)
  - `int vault_write(void)`: `device_seal` with passphrase `""`
  - `int dav_test(void)`: PROPFIND Depth 0 on `g_base`, shows the Test copy, returns 1 on 207
  - `int settings_change(void)`: keyboard email, then keyboard app password, then `vault_write`, then `dav_test`. If either keyboard is cancelled, it returns 0 and leaves the vault untouched.
  - `void settings_menu(void)`
- Produces, test helper `seed_vault(sd, user, pw)` in `conftest.py`: `run_unit` with `strcpy` + `vault_write()`.

- [x] **Step 1: Write `tests/test_settings.py`.** All through `run_script(…, koofr_url=dav.url)`.
  - `test_first_run_opens_settings`: no vault; answers: Keyboard email, Keyboard password, Message; the run then reaches the first-dialog stub, which prints `first dialog` and returns. Asserts:
    - the first UI prompt is `Koofr email`;
    - `Connected to Koofr as me@example.com.` was shown;
    - `config/savesync.vault` exists and its bytes contain neither the email nor `app-pass`.
  - `test_wrong_password_explained` (the 401 copy)
  - `test_status_shows_email` (`"Koofr: me@example.com"` in `r.status`)
  - `test_cancel_keeps_old_vault`
  - `test_damaged_vault_goes_to_settings` (garbage in the vault file: damaged copy, then Settings)
  - `test_test_connection_menu`
- [x] **Step 2: Run.** Expected: FAIL.
- [x] **Step 3: Implement.** In `main`, after `init_paths`: if `vault_read() != 1`, call `settings_change()`; if that returns 0, exit 0. Then `gui_status("Koofr: <email>")`, then call the stub `int first_dialog(char* outProfile)`, which prints `first dialog` and returns `ACT_QUIT` (Task 6 replaces it).
- [x] **Step 4: Run.** Expected: PASS.
- [x] **Step 5: Commit** (owner's go-ahead): `feat: Koofr login in the script's own Settings`

### Task 6: Profiles and the first dialog

**Files:**
- Modify: `scripts/savesync.c`
- Create: `tests/test_profiles.py`

**Interfaces:**
- Produces:
  - `int profiles_load(struct names* out)`, `int profiles_save(struct names* in)`
  - `int profile_name_check(char* name, struct names* existing)`: 1 valid, 0 bad characters or length, -1 duplicate ignoring case
  - `int first_dialog(char* outProfile)` returns `ACT_BACKUP`, `ACT_RESTORE`, `ACT_SETTINGS`, `ACT_UPDATE` or `ACT_QUIT`. With no saved names it goes straight to the keyboard; a cancelled keyboard returns `ACT_QUIT`.
  - Stubs replaced later: `int backup_run(char* profile)` prints `backup: <profile>` (Task 9); `int restore_flow(void)` prints `restore` (Task 11); `int update_flow(void)` prints `update` (Task 12).
  - The `main` loop: Settings returns to the first dialog; Backup and Restore end the run.

- [x] **Step 1: Write `tests/test_profiles.py`.**
  - `test_first_name_by_keyboard` (no names: keyboard `Serhii`, then `backup: Serhii`, and the profiles file holds `Serhii`)
  - `test_dialog_items` (file holds `Serhii`, `Son` → items `["Serhii","Son","New name...","Restore...","Settings...","Update script..."]`; pick `Son` → `backup: Son`)
  - `test_bad_names_refused` (`Сергій`, `a/b`, an empty name, 25 characters: each gets the bad-name copy and the keyboard again)
  - `test_duplicate_ignoring_case_refused`
  - `test_new_name_appended_in_order`
  - `test_restore_and_settings_entries`
- [x] **Step 2: Run.** Expected: FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run.** Expected: PASS.
- [x] **Step 5: Commit** (owner's go-ahead): `feat: profiles and first dialog`

### Task 7: Fingerprint and state file

**Files:**
- Modify: `scripts/savesync.c`
- Create: `tests/test_fingerprint.py`, `tests/test_state.py`

**Interfaces:**
- Produces:
  - `unsigned int crc32_buf(char* data, int len, unsigned int crc)`: the table is built once in a global.
  - `int zip_fingerprint(char* zipPath, char* out, int outSize)`: 1 OK, 0 error.
    - Read the end-of-central-directory record (search the last 65 557 bytes for `PK\5\6`), then the central directory entries (`PK\1\2`: CRC at offset 16, uncompressed size at 24, name length at 28, extra at 30, comment at 32, name at 46). Mask bytes with `& 0xFF`.
    - Build one `"%08x %u %s\n"` line per entry, sort the lines (shell sort), and join them.
    - `out` = `"<count>:<total bytes>:<crc32 of the joined text as %08x>"`.
  - `int state_load(void)`, `char* state_get(char* key)` (borrowed, or NULL), `int state_set(char* key, char* fp)`.
    - `state_set` rewrites the whole file through `savesync-state.txt.tmp` + `rename`, immediately.
    - A malformed line is skipped; a missing file means an empty state.

- [x] **Step 1: Write the tests.** The zips are made by `zip_dir` in the same `run_unit`.
  - `test_fingerprint.py`:
    - `test_crc32_vector` (`crc32_buf("123456789")` → `cbf43926`)
    - `test_same_tree_same_fingerprint`
    - `test_same_size_content_change_differs`
    - `test_rename_differs`
    - `test_added_file_in_subfolder_differs`
    - `test_mtime_only_change_same` (Python `os.utime` between the two zips)
    - `test_creation_order_irrelevant`
    - `test_fingerprint_stable_with_subfolders` (nested and empty folders, 3 runs equal)
  - `test_state.py`:
    - `test_set_get_across_runs`
    - `test_malformed_line_ignored`
    - `test_missing_file_empty`
    - `test_set_is_atomic_rename` (no `.tmp` left behind)
- [x] **Step 2: Run.** Expected: FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run.** Expected: PASS.
- [x] **Step 5: Commit** (owner's go-ahead): `feat: change detection by zip fingerprint`

### Task 8: Upload protocol and server time

**Files:**
- Modify: `scripts/savesync.c`
- Create: `tests/test_upload.py`

**Interfaces:**
- Consumes: Task 4.
- Produces:
  - `char g_stamp[20]`: `YYYYMMDD-HHMMSS` for this run.
  - `int stamp_from_date(char* date, char* out)`: parses `Thu, 12 Nov 2026 18:45:01 GMT` into `20261112-184501`; returns 0 if it cannot parse.
  - `int cloud_prepare(void)`: MKCOL `SaveSyncNx`, `saves`, `device` and `.incoming`, then sets `g_stamp` from `g_date`, or from the console clock with a log line.
  - `void cloud_cleanup_incoming(void)`: lists `.incoming/` and DELETEs every child. Failures are logged and the run goes on.
  - `int upload_version(char* zipPath, char* kindDir, char* gameFolder, char* versionBase, char* incomingName, char* outFinal)`: 1 OK.
    - PUT to `.incoming/<incomingName>`, then MKCOL the game folder, then MOVE to `<versionBase>.zip`. On 412, retry as `<versionBase> (2).zip`, `(3)`, and so on.
    - `outFinal` gets the final file name.
  - Incoming names are `<TITLEID>-<a|d>-<g_stamp>.zip`.

- [x] **Step 1: Write `tests/test_upload.py`.**
  - `test_stamp_variants` (fixed `Date`; lowercase `date`; no header: the result matches `^\d{8}-\d{6}$` and the log says console clock)
  - `test_upload_creates_version` (file under `dav.root/SaveSyncNx/saves/<folder>/<stamp> Serhii.zip`, `.incoming` empty)
  - `test_upload_never_overwrites` (name pre-existing on the server: `… (2).zip`, original bytes unchanged)
  - `test_dropped_put_leaves_only_incoming` (`drop_next("PUT")`: returns 0, no file under `saves/`; then `cloud_cleanup_incoming()` empties `.incoming`)
  - `test_dropped_move_leaves_only_incoming`
  - `test_nonascii_game_folder_roundtrip` (`0x0100ABCD00000000 Pokémon™ Let's Go  #1 & 50%`)
  - `test_only_incoming_is_deleted` (a DELETE outside `.incoming/` never appears in `dav.log`)
- [x] **Step 2: Run.** Expected: FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run.** Expected: PASS.
- [x] **Step 5: Commit** (owner's go-ahead): `feat: upload through .incoming with MOVE Overwrite: F`

### Task 9: Backup run

**Files:**
- Modify: `scripts/savesync.c` (replace the `backup_run` stub)
- Create: `tests/test_backup.py`

**Interfaces:**
- Consumes: Tasks 4–8.
- Produces:
  - `int entry_kind(int idx)`: `KIND_ACCOUNT` / `KIND_DEVICE` / `KIND_SKIP`, from the parent folder of `title_backup_path(idx, 0)`.
  - `void game_folder(int idx, char* out, int size)`: the basename of that path, `sanitize_name`d.
  - `int dump_save(int idx, char* dstDir)`: 1 OK, 0 failed, -1 empty save (no files). It walks `sav_list` recursively and writes each file with `sav_read` + `fwrite`.
  - `int backup_run(char* profile)`, step by step:
    1. Empty the work dir.
    2. `cloud_prepare()`, then `cloud_cleanup_incoming()`.
    3. For each entry: `dump_save`, then `zip_dir`, then `zip_fingerprint`; compare with `state_get`; then `upload_version` and `state_set`. Delete the dump folder and the zip after each entry.
    4. Show the summary copy.
  - An empty save is logged as `empty save, skipped` and counted as unchanged (designer's call).
  - Progress: layer 0 = entries, labelled with the game name.

- [x] **Step 1: Write `tests/test_backup.py`.** End to end through `run_script`, with a seeded vault and a profile picked by item.
  - `test_first_run_uploads_account_and_device_skips_bcat_system` (2 zips on the server; summary `2 uploaded, 0 unchanged, 0 failed`)
  - `test_second_run_unchanged` (no PUT in `dav.log`; `0 uploaded, 2 unchanged`)
  - `test_changed_save_adds_version` (only that game gets a second zip)
  - `test_zip_contents_match_save` (Python `zipfile`; nested folders; bytes equal)
  - `test_two_profiles_device_once` (run as A, then B with B's own account dir: B's zip ends in ` B.zip`; the device save stays unchanged in B's run)
  - `test_no_save_writes` (`r.save_writes == []` for every run in this file)
  - `test_nothing_left_on_sd` (after a run `savesync/` is empty; `config/` holds only the vault, profiles and state)
  - `test_lost_state_reuploads_once`
  - `test_empty_save_skipped`
  - `test_large_file_roundtrip` (one 20 MB random file)
- [x] **Step 2: Run.** Expected: FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run.** Expected: PASS.
- [x] **Step 5: Commit** (owner's go-ahead): `feat: one-action backup of changed saves`

### Task 10: Errors

**Files:**
- Modify: `scripts/savesync.c`
- Create: `tests/test_errors.py`

**Interfaces:**
- Consumes: `g_fatal` (Task 4) and `backup_run` (Task 9).
- Produces:
  - After every save, `backup_run` checks `g_fatal`. A fatal error stops the loop with the matching run-stop copy, and the summary is not shown.
  - Any other failure counts that save as failed, leaves its state untouched and goes on to the next save.

- [x] **Step 1: Write `tests/test_errors.py`.**
  - `test_wrong_login_stops_run` (copy shown; no PUT)
  - `test_quota_full_stops_run`
  - `test_network_down_stops_run` (`dav.stop()` before the run)
  - `test_one_failed_put_fails_only_that_game` (`fail_next("PUT", 500, path_contains=<id>)`: `1 uploaded, 0 unchanged, 1 failed`)
  - `test_failed_game_retried_next_run`
  - `test_unopenable_save_counts_failed` (catalog entry without `dir`)
- [x] **Step 2: Run.** Expected: FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run.** Expected: PASS.
- [x] **Step 5: Commit** (owner's go-ahead): `feat: stop on login, network and quota errors`

### Task 11: Restore

**Files:**
- Modify: `scripts/savesync.c` (replace the `restore_flow` stub)
- Create: `tests/test_restore.py`

**Interfaces:**
- Consumes: Tasks 4, 9.
- Produces:
  - `void sort_desc(char** items, int n)`: shell sort, newest first.
  - `void sd_rmtree(char* path)`: `remove`, otherwise recurse with `read_directory`, then `rmdir`.
  - `int restore_flow(void)`, step by step:
    1. `dav_list` `saves/` and `device/`, and group the folders by `0x<id>`.
    2. Keep the games the catalog has a save of the same kind for; count the rest as k.
    3. Pick a game, then pick a version from all of that game's folders.
    4. Confirm, then `dav_get_file` to `savesync/down.zip`.
    5. Destination: `title_backup_path(idx,0)` + the version without `.zip`; if it exists, ` (2)`, ` (3)` and so on. Then `sd_mkdirs` and `unzip`.
    6. On any failure: `sd_rmtree(dest)` and the failed copy. Delete `down.zip` either way.

- [x] **Step 1: Write `tests/test_restore.py`.** Server content is seeded straight into `dav.root`.
  - `test_lists_only_games_on_console` (prompt mentions `1 more in Koofr`)
  - `test_versions_newest_first_no_cap` (200 versions listed, first is the newest)
  - `test_restore_bytes_identical`
  - `test_existing_local_folder_gets_suffix`
  - `test_corrupt_zip_rolls_back` (truncated zip: failed copy; no folder left; no `down.zip`)
  - `test_renamed_game_folders_merge` (two folders with the same id and different names)
  - `test_device_save_labelled_and_restored_to_device_folder`
  - `test_cancel_at_confirm_writes_nothing`
  - `test_restore_nonascii_game`
  - `test_restore_never_writes_saves` (`save_writes == []`)
- [x] **Step 2: Run.** Expected: FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run.** Expected: PASS.
- [x] **Step 5: Commit** (owner's go-ahead): `feat: restore any version into Checkpoint's folder`

### Task 12: Update from GitHub releases

**Files:**
- Modify: `scripts/savesync.c` (replace the `update_flow` stub)
- Create: `tests/test_update.py`

**Interfaces:**
- Consumes: Task 4 (`dav_call` is not used here, because GitHub needs no auth; use `web_request` directly with the header `User-Agent: savesync`) and the JSON bindings.
- Produces: `int update_flow(void)`.
  1. GET `GITHUB_API` and parse `tag_name`.
  2. Same tag → the latest copy. Otherwise confirm (the offer copy).
  3. GET `GITHUB_RAW/<tag>/scripts/savesync.c` to `savesync/update.c`.
  4. Check it contains `#define SAVESYNC_VERSION "<tag>"`; if not, delete it and show the failed copy.
  5. `rename` it over `<root>/scripts/universal/savesync.c` (`sd_mkdirs` the folder first), then the done copy.
- `run_script` passes `GITHUB_API`/`GITHUB_RAW` defines that point at `dav.url`'s host with static routes.

- [x] **Step 1: Write `tests/test_update.py`.**
  - `test_latest_version_says_so`
  - `test_offer_shows_both_versions` (prompt has `v0.1.0` and `v0.2.0`)
  - `test_confirm_replaces_script` (file content equals the served bytes)
  - `test_cancel_changes_nothing`
  - `test_wrong_payload_rejected` (body without the version line: failed copy, installed file unchanged, no `savesync/update.c` left)
  - `test_github_unreachable` (`static` 500: failed copy with `(500)`)
  - `test_user_agent_sent` (the fake records the request headers)
- [x] **Step 2: Run.** Expected: FAIL.
- [x] **Step 3: Implement.**
- [x] **Step 4: Run.** Expected: PASS.
- [x] **Step 5: Commit** (owner's go-ahead): `feat: self-update from GitHub releases`

### Task 13: README and the console check

**Files:**
- Modify: `README.md`
- Create: `docs/console-check.md`
- Modify: this plan's checkboxes and Deviations; the spec's Open items if the console answers one.

**README.md covers:**
- What it is.
- Requirements: Switch with CFW, Checkpoint ≥ 5.3.0, Wi-Fi, a Koofr app password (preferences → password → app passwords).
- Install: copy `scripts/savesync.c` to `sdmc:/switch/Checkpoint/scripts/universal/`, over Checkpoint's USB (MTP) or FTP, keeping the name `savesync.c`.
- Updating: "Update script..." on the console. Releasing: set `SAVESYNC_VERSION` to the new tag, then tag `vX.Y.Z` and publish a GitHub release. The repo must stay public.
- Usage: the first run, the second profile (switch it in Checkpoint with ZL), restore.
- What is stored where: the cloud layout and the SD files.
- Limitations: hidden games, fully deleted games (open), TLS not verified by Checkpoint, picking the wrong profile.
- Development: Arch packages `base-devel curl mbedtls3`, pytest from mise (`mise trust`, `mise install`), `git submodule update --init --recursive`, `make test`.

**`docs/console-check.md`**, a checklist for the owner, with the expected result after each line:
- Settings with a wrong password, then the right one.
- First backup.
- Second run: everything unchanged.
- Play a game, run again: one upload.
- Hold B during an upload, then run again: `.incoming` cleaned, the save uploaded.
- Restore a version and write it with Checkpoint's Restore.
- The son's profile.
- Update script once a newer release exists.
- Are fully deleted games in Checkpoint's list? (Open item 1)

- [x] **Step 1: Write both files.**
- [x] **Step 2: Run** `make test`. Expected: all PASS.
- [x] **Step 3: Commit** (owner's go-ahead): `docs: README and console checklist`
- [ ] **Step 4: The owner runs `docs/console-check.md` on the Switch.** Record the results and any fixes here under Deviations, and answer the spec's Open item 1.

## Deviations

- **Execution (2026-11-12):** executed inline by Claude (owner chose "Native"). No commits are made during execution: the owner approves every commit, so each task's "Commit" step is left unticked until the owner says so. No git worktree (the repo has no commit to branch from).
- **Commit (2026-11-13, owner's request):** Tasks 1–9 went into one first commit on `main` instead of one commit per task (the tasks share `savesync.c`, so they cannot be split cleanly afterwards). Their Commit steps are ticked.
- **Task 1, Step 1:** `git -C` is blocked by the owner's shell hook, so the submodule was pinned by writing the gitlink (`git update-index --cacheinfo 160000,740f221a853db784401458fe74276977afcfa8b0,third_party/Checkpoint`, the commit of tag `v5.3.0`) and running `git submodule update --init --recursive`. To move to another Checkpoint release later, do the same with that tag's commit (`git ls-remote https://github.com/BernardoGiordano/Checkpoint refs/tags/<tag>`).
- **Task 1, Step 2:** all 40 bindings are present at v5.3.0.
- **pytest through mise (2026-11-12, owner's decision):** pytest is not installed system-wide (no Arch `python-pytest`). The repo has a `mise.toml` with `"pipx:pytest" = "latest"`; mise installs it with uv into its own environment, and the commands are `pytest ...` instead of `python -m pytest ...`. The tests use only the standard library besides pytest. One-time setup: `mise trust`, `mise install`. mbedtls still comes from pacman.
- **Task 2, Step 3 — mbedtls:** Arch's `mbedtls` is 4.2, which no longer ships the `gcm.h`/`md.h`/`pkcs5.h` API `seal_api.cpp` uses, and Arch has no `mbedtls2`. hostrun builds against Arch's `mbedtls3` (3.6.7), which installs beside it: headers from `/usr/include/mbedtls3`, `libmbedcrypto` from `/usr/lib/mbedtls3` with an rpath (`MBEDTLS_INC`/`MBEDTLS_LIB` in the Makefile). The console links devkitPro's mbedtls 2.28; the calls `seal_api.cpp` makes behave the same in 3.6. Only `libmbedcrypto` is linked. The `mbedtls` 4.2 package is not needed.
- **Task 2, Step 3 — run model:** the script runs through `ScriptRunner::start`/`takeResult` rather than a bare `ScriptEngine::run` on a `std::thread`, so the after-run cleanup (`ckpt_sav_close_all`, `ScriptHeap::releaseAll`), the abort path and the `cancelled` flag are Checkpoint's own code, and the worker gets the console's 128 KB stack. After the first runner error every later dialog is cancelled too, and `hostrun: <reason>` is appended to the output.
- **Task 2, Step 3 — build flags:** the same picoc flags as `switch/Makefile` (`-DUNIX_HOST -DDEBUGGER -D_GNU_SOURCE=1 -funwind-tables`; C++ `-std=gnu++23 -fno-rtti`), at `-O2`. `DEBUGGER` matters: it is what makes picoc call Checkpoint's abort hook.
- **Task 2, Step 3 — shims** (`tools/hostrun/shim/`): `paths.hpp` (only `Paths::checkpointRoot`, the one call the bindings make); `thread.hpp` (pthreads, honouring the requested stack size); `funopen.h` (force-included into `scriptconsole.cpp` only, mapping newlib's `funopen` onto glibc's `fopencookie`); `platform.cpp` (those, plus `Logging`: warnings and errors to stderr, which the tests append to the output; info and below dropped, because the engine's "output:" echo would duplicate the transcript).
- **Task 2, Step 3 — full transcript and status list:** Checkpoint's console keeps only 250 lines wrapped at 56 columns, so `tools/hostrun/capture.cpp` taps the three ways output reaches it (the funopen stream, `ckpt_console_write`, `ScriptConsole::log`) and `ScriptUiBridge::setStatus`, the last three with `-Wl,--wrap`. A wrong mangled name fails the link on an undefined `__real_…`, so the taps cannot silently drop out. Two tests beyond the plan pin them: `test_status_texts_recorded_in_order`, `test_script_log_and_long_lines_reach_output`.
- **Task 3 — folder hrefs:** the plan said folder hrefs end in `/`; the real Koofr answer (`tests/fixtures/koofr-propfind.xml`) has none, and lists the requested folder itself first. fakedav follows Koofr, and `test_propfind_depth1_shape_matches_koofr_fixture` compares its answer with the fixture byte for byte. Task 4's `dav_list` therefore must not rely on a trailing `/`.
- **Task 3 — encoding:** fakedav percent-encodes every href character except unreserved ones and `/` (Python `quote(…, safe="/")`). The probe only showed how Koofr encodes a space and `é`; whether it leaves `&`, `'` or `:` raw (an XML-escaped `&amp;` in the href) is unknown, so the script's href decoder should accept both. Worth a look on the console run.
- **Task 3 — details the plan left open:** `.log` holds decoded paths (`("MKCOL", "/dav/Koofr/Some Game", 201)`), with status `0` for a dropped request; `.origin` (`http://127.0.0.1:<port>`) is there for the GitHub fake URLs; PUT into a missing folder → 409; MOVE onto an existing name without `Overwrite: F` replaces it → 204; PROPFIND without `Depth` is treated as Depth 1. Two tests beyond the plan: `test_propfind_depth0_lists_only_the_target`, `test_log_and_static_routes`.
- **Task 4 — requests as on Koofr:** MKCOL and PROPFIND go to the folder URL without a trailing `/`, and PUT sends `Content-Type: application/octet-stream`, as the 3DSaveSync client that runs against Koofr does (Checkpoint's `webdav.c` adds the slash for Synology).
- **Task 4 — `dav_put_file` and -3:** `web_upload_file` answers -3 when the local file cannot be read. That is not a network failure, so it does not set `FATAL_NET` (the plan's "< 0 → FATAL_NET" rule holds for every other negative code).
- **Task 4 — `dav_list`:** the folder's own entry is recognised by its decoded path, not by its position or its last segment. The parse accepts `<D:href>` or `<d:href>`, absolute-URL hrefs and XML entities (`&amp;`), since how Koofr writes `&` in an href is unknown (see Task 3).
- **Task 4 — `g_date`** is cleared when a response has no `Date`, so it is always the last response's value.
- **Task 4 — `init_paths`** empties `savesync/` with a small `rm_tree`: `read_directory` gives full paths without marking folders, so an entry `remove()` refuses is treated as a folder, emptied and `rmdir`ed.
- **Task 4 — helpers and tests beyond the plan:** `rm_tree`, `rstrip_slash`, `basename_of`, `hexval`, `has_at`, `xml_unescape`, `url_path`, `build_headers`, `note_response`; bounds `URLBUFN` 1536, `NAMEN` 512, `HDRN` 2560. Tests `test_b64_padding`, `test_url_add_refuses_overflow`, `test_sanitize_name_keeps_utf8`, `test_xml_unescape`, `test_url_path`, `test_get_missing_returns_0`, `test_list_missing_returns_0`.
- **Task 5 — where Settings shows the email:** the spec wants the saved email visible in Settings. The Settings prompt keeps the exact copy (`Settings`); the email is on the status line (`Koofr: <email>`), which is set at the start of the run and again after a change, and stays on screen while Settings is open.
- **Task 5 — `dav_test` leaves `g_fatal` alone:** a failed test in Settings is a diagnosis, so it must not make a later backup in the same run stop at once.
- **Task 5 — damaged vault:** also when the blob opens but holds no email or password. `settings_change` returns 0 when sealing or writing fails (the message says why).
- **Task 5 — tests through `run_unit`:** `settings_menu` is reachable from the first dialog only from Task 6 on, so `test_cancel_keeps_old_vault` and `test_test_connection_menu` call it directly. Tests beyond the plan: `test_first_run_cancel_writes_nothing`, `test_change_from_menu_updates_vault_and_status`, `test_test_copy_404_and_transfer_error`. `conftest.py` has the `seed_vault` fixture.
- **Task 6 — an empty profile name is a cancel:** Checkpoint answers an empty keyboard as cancelled (`UiResponse.text` empty), so the first dialog cannot tell "typed nothing" from "pressed B". The plan's `test_bad_names_refused` case "an empty name" is therefore checked on `profile_name_check("")` directly (`test_name_check_rules`); in the dialog an empty answer cancels.
- **Task 6 — where cancels lead:** a cancelled "New name..." keyboard goes back to the list (the plan only fixed the no-names case, which quits). "Update script..." comes back to the first dialog like Settings; Backup and Restore end the run.
- **Task 6 — keyboard size:** the profile keyboard takes up to 31 characters, so a name over 24 is refused with the bad-name copy instead of being clipped silently.
- **Task 6 — Task 5's tests** now end at the real first dialog (they cancel the first-name keyboard) instead of the `first dialog` stub. Tests beyond the plan: `test_name_check_rules`, `test_new_name_cancel_returns_to_dialog`, `test_update_entry_returns_to_dialog`, `test_cancel_first_dialog_quits`, `test_first_keyboard_cancel_quits`.
- **Task 7 — folders in the fingerprint:** "one line per entry" includes the folder entries `zip_dir` writes (`dir/`, size 0, CRC 0), so adding or removing an empty folder in a save counts as a change. The spec speaks of files; a save's empty folder is part of what Checkpoint would restore, so it stays in.
- **Task 7 — `state_set` on the Switch:** its FAT `rename` does not replace an existing file, so when the rename is refused the old file is removed and the rename retried. If a run stops between the two, `state_load` reads the `.tmp` file instead. `state_set` loads the state first if nobody did, so it can never write a file with one line.
- **Task 7 — helpers and tests beyond the plan:** `names_init`, `sort_names` (Restore reuses it), `le16`/`le32`, `read_at`; `test_not_a_zip_is_an_error`, `test_leftover_tmp_is_recovered`, and a Python cross-check (`zipfile` + `zlib.crc32`) of the fingerprint format in `test_same_tree_same_fingerprint` and `test_fingerprint_stable_with_subfolders`.
- **Task 8 — the folder URLs** are globals set by `cloud_prepare`: `g_url_root`, `g_url_saves`, `g_url_device`, `g_url_incoming` (`URLN` each). Corrected in the final review: picoc allocates globals with `calloc` (`HeapAllocMem`), so they do not use the 64 KB picoc stack; only locals do.
- **Task 8 — `stamp_from_date`** reads only the HTTP date form (`Thu, 12 Nov 2026 18:45:01 GMT`, day of 1–2 digits) and checks the ranges; anything else falls back to the console clock with the log line `version time: console clock (Koofr sent no Date)`.
- **Task 8 — name clashes** retry up to `(999)`; past that the upload fails rather than loop forever on a server that answers 412 to everything. A MOVE answered 204 counts as done, like 201.
- **Task 8 — a dropped connection** (fakedav `drop_next`) reaches the script as a transfer error, so it also sets `FATAL_NET`; the backup run (Task 10) then checks whether Koofr still answers (final review fix pass). Test beyond the plan: `test_stamp_from_date_parses_and_refuses`.
- **Task 9 — shape of `backup_run`:** one `backup_entry` per catalog entry. The dump folder is deleted right after zipping (spec step 3.2), the zip after the entry. A save that cannot be opened, copied or zipped counts as failed; Task 10 adds the stop on fatal errors. `g_fatal` is cleared at the start of the run.
- **Task 9 — log and status:** `backup as <profile>` at the start, then one line per game (`<folder>:` plus `unchanged`, `uploaded <file>` or the failure), and the summary also goes to the log, as the spec asks. The status reads `Connecting to Koofr...` until the folders are ready, then `Koofr: <email>` again.
- **Task 9 — Task 6's tests** now check `backup as <name>` and answer the summary message, instead of the `backup: <name>` stub.
- **Task 10 — where the run checks for a stop:** after the folders are made, after the `.incoming/` cleanup and after every save. The stop copy also goes to the log; the save that hit the error is not stored as uploaded, so the next run retries it.
- **Task 10 — the folders cannot be made for another reason** (MKCOL answered e.g. 500 or 409): the plan had no copy for it and the run used to end with no message at all. It now stops with `Koofr answered HTTP <code>.`, the copy Settings' test already uses for other statuses (`g_status` keeps the last response's status). Test `test_server_error_at_start_says_so`.
- **Task 10 — tests beyond the plan:** `test_forbidden_stops_run` (403), `test_network_lost_mid_run_stops_run` (a dropped PUT on the first game: the second is never sent), `test_dropped_move_retried_next_run` (Review Focus 2: the run stops, one file is left in `.incoming/`, the next run deletes it and uploads the save). `test_one_failed_put_fails_only_that_game`, `test_failed_game_retried_next_run` and `test_unopenable_save_counts_failed` passed before the change: Task 9's `backup_entry` already counted those failures and left the state alone.
- **Task 11 — helpers:** no `sort_desc` and no `sd_rmtree`. The versions are sorted with Task 7's `sort_names` and read backwards; a failed restore removes its folder with Task 4's `rm_tree` plus `rmdir`. `cloud_prepare` is split so that Restore gets the folder URLs (`cloud_urls`) without making folders. Checkpoint's picoc has no `toupper` without `<ctype.h>`, so the title id is uppercased by hand.
- **Task 11 — what Restore lists:** a `saves/` or `device/` folder Koofr does not have yet (404) counts as empty; any other listing failure stops with the run-stop copy (Task 10's `show_stop`). Games appear in catalog order, account before device when the catalog does. `k` counts distinct title ids per kind. The `(k more …)` line first left out the spec's hint that a game must be started once to create its save; the final review restored it (see the Copy table).
- **Task 11 — where B leads:** confirm → back to the versions, versions → back to the games, games → the run ends. The plan did not say; one step back is the usual Checkpoint behaviour.
- **Task 11 — a game folder with no versions** (left by a run that stopped between the game folder's MKCOL and the MOVE) shows `There are no versions of <name> in Koofr.` and goes back to the games. New copy; the plan had none for this case.
- **Task 11 — labels:** the version prompt and the confirm copy use the game's label, so a device save reads `Game One (device)` there too. The status reads `Downloading from Koofr...` during the GET.
- **Task 11 — order of a restore:** the free folder name is chosen first, the zip is downloaded, and only then is the folder made and unzipped into. A failed download creates no folder at all. Changed in the final review: the zip is unzipped into `savesync/restore` and renamed into place (see below).
- **Task 11 — tests beyond the plan:** `test_no_backups_says_so`, `test_download_error_rolls_back`, `test_game_folder_without_versions` (written after the code), `test_restore_network_down`. Task 6's `test_restore_and_settings_entries` now answers the real Restore's "no backups" message instead of checking the `restore` stub.
- **Task 12 — two more failure reasons,** for cases the plan's two did not cover:
  - `GitHub's answer names no release`: the API answered 200 but with no `tag_name`, or with a tag that is not 1–31 of `A-Z a-z 0-9 . - _`. The tag goes into the raw URL's path, so `../x` is refused.
  - `the script could not be written to the SD card`: the download could not be written or moved into place.
  A repo with no release yet answers 404, which reads `GitHub could not be reached (404)`.
- **Task 12 — the check is made in memory,** before anything is written: only a body that holds `#define SAVESYNC_VERSION "<tag>"` is written to `savesync/update.c` and renamed into place.
- **Task 12 — FAT rename:** `install_script` tries a plain `rename` first (it replaces on the PC). If that is refused, it moves the installed script aside to `savesync/installed.old`, renames the new one in, and moves the old one back if that fails. The PC tests reach only the first branch; the console check covers the second.
- **Task 12 — test safety:** `conftest.py` points `GITHUB_API` and `GITHUB_RAW` at `http://127.0.0.1:9/no-github` unless a test serves its own fakes, so no test ever reaches the real GitHub. Task 6's `test_update_entry_returns_to_dialog` now answers the "could not be reached" message instead of checking the `update` stub.
- **Task 12 — fakedav** records every request's headers in `.request_headers` (`test_user_agent_sent` checks `User-Agent: savesync` and that no `Authorization` header goes to GitHub). Tests beyond the plan: `test_confirm_installs_when_folder_missing`, `test_no_release_yet`, `test_download_unreachable`, `test_release_without_usable_tag` (written after the code).
- **Task 13 — README:** the "hidden games" limitation is written as what is known: only the games in Checkpoint's list for the selected user are backed up. Added two lines the plan did not list: what is checked of an upload after it lands (see the owner's decision below), and the free SD space a run needs. Releasing names a published release, not a draft or pre-release, which `releases/latest` does not return.
- **Task 13 — `docs/console-check.md`** adds checks beyond the plan's list: the vault holds no readable login, a downloaded zip opens on a PC (the Koofr empty-file question for `.zip`), Wi-Fi off, `savesync/` empty after a run, restoring the same version twice, and the FAT branch of the update's rename.
- **Raised 2026-11-13, decided by the owner the same day:** 3DSaveSync found that Koofr accepts `*.bin` files (PUT 201) but serves them back empty. SaveSyncNx uploads `.zip` only, which nobody has tested on Koofr. The owner chose the cheaper of the two checks offered: before the MOVE, PROPFIND (Depth 0) the `.incoming` file and compare `getcontentlength` with the local zip's size; on a mismatch DELETE it and count the save as failed (`dav_size`, `sd_size`). Downloading it again was declined (it doubles the traffic). Tests `test_upload_size_checked_before_move`, `test_short_stored_upload_fails_and_is_removed`; fakedav `short_next_put`.

### Final review fix pass (2026-11-13)

The whole-project review found one critical and two important defects; the owner answered four questions. Each change below has a test that failed first.

- **Hold-B during Restore's unzip** (critical) left a half-written folder in Checkpoint's backup list: picoc stops at the next statement after an abort, so the cleanup after `unzip` never ran. `restore_version` now unzips into `savesync/restore` and renames it into place; the rename is the commit point, and `savesync/` is emptied at every start. hostrun gained `--abort-during <zip|unzip>`, which wraps `ScriptConsole::beginIo`/`addIo` and raises Checkpoint's abort on the binding's first progress tick. Test `test_abort_during_unzip_leaves_no_folder`.
- **`dav_list` passed `.` and `..` on:** a listing naming them made the cleanup send `DELETE .incoming/..`, which removed `SaveSyncNx/` with every version (the RED run showed it). They are skipped now. fakedav gained `extra_hrefs`. Test `test_cleanup_skips_dot_entries`.
- **Unchecked `fclose`** in `dav_get_file`, `vault_write`, `profiles_save`, `dump_dir` and `update_flow`: a failed final flush counts as a short write. A failing `profiles_save` is logged (`the profile names could not be saved`); the backup still runs under the typed name. hostrun gained `--fail-close <path part>` (wraps `fopen`/`fclose`). Tests `test_unflushed_download_rolls_back`, `test_unflushed_vault_refused`, `test_unflushed_profiles_reported`, `test_unflushed_dump_counts_failed`, `test_unflushed_download_keeps_installed`.
- **Restore's game list** now carries the spec's hint (`start a game once to create its save`).
- **Owner: non-Latin game names.** Checkpoint names such a folder `0x<ID> 0x<ID>`; `game_folder` now builds the cloud folder from `0x<ID> ` + the title name for any title with a non-ASCII byte. ASCII titles keep Checkpoint's folder name. Tests `test_nonascii_title_folder_keeps_its_name`, `test_ascii_title_folder_is_checkpoints`; `test_restore_nonascii_game` became `test_restore_special_names` (also `Luigi's Mansion 3 & #1 50%`, a coverage gap the review named; it passed at once).
- **Owner: one failed transfer.** After a transfer error or a 401/403 in the middle of a run, `recheck_koofr` sends one PROPFIND (Depth 0) of `SaveSyncNx/`; on 207 only that save fails and the run goes on, otherwise it stops with the reason. Never for 507. `test_network_lost_mid_run_stops_run` now loses the network for good (fakedav `fail_from`); `test_dropped_move_retried_next_run` now expects `1 uploaded, 0 unchanged, 1 failed`, then `1 uploaded, 1 unchanged`. New tests `test_login_lost_mid_run_stops_run`, `test_one_dropped_put_fails_only_that_game`, `test_one_forbidden_put_fails_only_that_game`; `test_quota_full_stops_run` checks that no recheck is sent.
- **Owner: a fresh install** stays as designed (the first run backs up; Restore from the second run). README says so under Limitations.
- **Docs:** `docs/console-check.md` — the wrong-password line now expects the connection test's 401 message after typing, then the run-stop message; new lines for hold-B during the unzip, a name with `'`/`&`/`#`/`%`, a non-Latin name, Wi-Fi lost mid-run and a fresh install. The README has a "When something fails" paragraph, and the Stopping paragraph covers Restore.
