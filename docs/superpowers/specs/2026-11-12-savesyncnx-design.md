# SaveSyncNx — Design

Date: 2026-11-12
Status: approved by the owner 2026-11-12.

## Intent

The owner's Nintendo Switch is banned, so Nintendo's cloud saves are not available, and right now no
Switch save is backed up anywhere. The saves live in the console's internal memory: if the console
breaks or is lost, every save goes with it.

SaveSyncNx backs up the save of every game on the Switch to Koofr over WebDAV with one action. It
uploads a game only when its save changed, keeps every version in the cloud, leaves nothing piling
up on the console, and downloads any version back so Checkpoint can restore it.

It is the Switch sibling of 3DSaveSync (`~/dev/3ds/3DSaveSync`): same owner, same Koofr account,
same principles (launch-and-forget backup, never delete in the cloud, restore through Checkpoint).
Unlike 3DSaveSync it is **not a standalone app** but a **script for Checkpoint**.

### Why a Checkpoint script

Researched with the owner on 2026-11-12:

- **JKSV** (rejected): its "back up everything" re-copies every save every time without checking
  whether it changed (the owner checked on the console), and its auto-upload has no destination in
  the settings UI (it needs a hand-made `webdav.json` with the password in plaintext on the SD card).
- **Checkpoint 5.3.0** (2026-10-13) has a scripting engine and ships a `webdav` script that uploads
  Checkpoint backups to WebDAV, keeps the server password sealed, and downloads a backup back. It
  lacks what the owner needs: going from the console's saves to the cloud in one action, skipping
  unchanged saves, not leaving backup copies on the SD card, and a restore that still works after
  years of versions (see "Why not the bundled restore").
- **Forking Checkpoint** (rejected): a script survives Checkpoint updates; a fork would have to be
  rebuilt and kept in sync with an actively developed upstream.

## Goals

- **One action.** Checkpoint → Minus → Scripts → `savesync` → confirm → pick whose profile it is →
  everything else runs with no further input.
- **Only changes are uploaded.** A save that is the same as the last one uploaded is reported as
  "unchanged" and nothing is sent. Saves of games deleted long ago (the save stays on the console)
  are uploaded once and are "unchanged" from then on.
- **Nothing accumulates on the console.** No backup copies are left on the SD card. During a run
  there are temporary files for the save being processed; between runs only small state and config
  files remain.
- **Every version stays in the cloud.** The script never deletes or overwrites a complete version in
  the cloud (owner: same rule as 3DSaveSync). Old versions are deleted by hand, if ever, in Koofr's
  web interface.
- **Own Restore** (owner's choice): pick a game, then a version, newest first, with no limit on the
  number of games or versions; the game is found by its title id, not its name. The version is
  downloaded into Checkpoint's backup folder for that game, and the owner writes it into the game
  with Checkpoint's Restore.
- **Two profiles.** There is one user profile now and there will be a second (the owner's son). Each
  profile's account saves are backed up and labelled with that profile's name.
- **Credentials never in plaintext on the SD card.**
- **Self-update** (owner's choice): the first copy goes onto the console by hand (Checkpoint's USB
  mode or FTP); after that, "Update script..." in the first dialog fetches the latest GitHub release
  and replaces the script after the owner confirms. Never automatic.

## Non-goals

- Writing saves into games. Checkpoint's Restore does that.
- Starting automatically when Checkpoint opens. Scripts start only from the Scripts menu; the owner
  accepted a few button presses.
- Backing up both profiles in one run. The script sees only the profile selected in Checkpoint; the
  second profile is a second run after switching the profile in Checkpoint (ZL).
- Detecting the current profile automatically. The scripting API has no call for it; the owner chose
  to be asked at start (adding the call to Checkpoint would mean a fork).
- BCAT saves (downloaded game news/events) and system saves (the console's own data) (owner's
  choice). Only account and device saves are backed up.
- Deleting versions from the script. Done by hand in Koofr's web interface, if ever.
- Automatic pruning or any retention policy.
- The 3DS. 3DSaveSync covers it (designer's call: not designed or tested for a 3DS).
- Protecting the credentials from other homebrew on the same console (see Constraints, "Sealed
  storage").

## Anti-goals

These make the script a failure even if everything else works:

- **It deletes or overwrites a complete version in the cloud.** A broken save that gets uploaded as
  the newest version can only be rescued by an older one. The only automatic deletion allowed is the
  script's own unfinished uploads (see "Upload").
- **It changes anything in a game's save.** Saves are only read. The script never calls `sav_write`,
  `sav_delete`, `sav_mkdir`, `sav_rmdir`, `sav_rename` or `sav_commit`.
- **The Koofr password is on the SD card in plaintext.**
- **Backups pile up on the SD card**, or the same unchanged save is uploaded again run after run.
- **Restore writes into an existing backup folder** or leaves a half-written one that Checkpoint
  would offer for restoring.

## Decided by the owner

| Area | Decision |
|---|---|
| Console | Switch with custom firmware, banned (no Nintendo cloud saves) |
| Form | A script for Checkpoint, not a standalone app, not a Checkpoint fork |
| Cloud | Koofr over WebDAV (same account as 3DSaveSync) |
| What triggers a backup | The owner runs the script; no further input after picking the profile |
| Unchanged saves | Not uploaded again |
| Local copies | None kept on the SD card |
| Cloud versions | Every version kept; never deleted or overwritten automatically |
| Change detection | Left to the designer ("whichever is most efficient") |
| Lost state file | The next run uploads everything once (accepted) |
| Profiles | Two soon; the script asks whose profile it is at every run |
| Save kinds | Account and device saves; not BCAT, not system |
| Restore | The script's own: newest first, no cap, game matched by title id; downloads into Checkpoint's folder, Checkpoint writes it into the game |
| Credentials entry | In the script's own Settings, typed on the console (no PC needed) |
| Delivery | First copy by hand; then "Update script..." from the latest GitHub release, on confirm (the repo is public; the owner accepted that Checkpoint does not verify HTTPS certificates) |
| PC tests | The real Checkpoint 5.3.0 interpreter and bindings on the PC with a fake console and a fake Koofr; pytest, installed by mise from the repo's `mise.toml` |
| Version control | git, remote `git@github.com:NuclleaR/SaveSyncNx.git`; commits only on the owner's go-ahead, no AI attribution |
| Fresh install (2026-11-13) | Stays as designed: with no profile names the first run goes to the keyboard and backs up; Restore is reachable from the second run |
| Non-Latin game names (2026-11-13) | The cloud folder is `0x<TITLEID> ` + the real title name, not Checkpoint's `0x<ID> 0x<ID>` |
| One failed transfer (2026-11-13) | After a transfer error or 401/403 in the middle of a run, check whether Koofr still answers; if it does, only that save fails and the run goes on (never for 507) |
| Upload check (2026-11-13) | Before the MOVE, compare the size Koofr reports for the uploaded file with the local zip; on a mismatch delete it from `.incoming/` and fail the save |

## Constraints found during research (2026-11-12)

From Checkpoint's source (`master`, cloned 2026-11-12) and its `scripts/README.md`,
`scripts/webdav.md`, `scripts/common/universal/webdav.c`. The builder must re-check every API call
against the **v5.3.0** tag, because the owner's console runs a release, not `master`.

**Scripting engine.**
- Scripts are single `.c` files interpreted by picoc, placed in
  `sdmc:/switch/Checkpoint/scripts/universal/`; the file name without `.c` is the menu entry.
- picoc limits: no `const`/`volatile`, no 64-bit integers (title ids are 16-hex strings), no pointer
  arithmetic, no adjacent string-literal concatenation, constant array bounds only, structs at file
  scope only, callees defined before callers. Interpreter stack 64 KB; at most 8 open save handles.
  Interpreted code is slow: per-byte work in script code (hashing a save) is out.
- No call tells the script which user profile is selected. On Switch the catalog holds the selected
  profile's account saves, plus the BCAT, device and system saves, appended to every profile's list.
- `title_backup_path(idx, 0)` tells the save kinds apart: `…/saves/` (account), `…/device/`,
  `…/bcat/`, `…/system/`. An account save and a device save of the same game share the title id, so
  `title_find(id)` is not enough to pick one; match on id **and** kind.
- Checkpoint lists a title only if the console still returns its name and icon
  (`nsGetApplicationControlData` in `switch/source/titleprobe.cpp`), and not if it is hidden in
  Checkpoint's settings. Saves outside the catalog cannot be reached by a script: hidden games are
  not backed up (a known limitation), and fully deleted games may be missing (Open item 1).
- No hash function in the API. `zip_dir` (native, fast) writes a store-only zip that records a CRC32
  and size for every file; `unzip` checks the CRCs; `sav_read` returns a whole file in one native
  allocation; `fwrite` and `memcmp` are native.
- `gui_keyboard` has no small length cap on Switch (63 characters on 3DS only).
- `web_upload_file` streams a file from SD (any method, e.g. `PUT`); `web_request` takes any verb
  (`MKCOL`, `PROPFIND`, `MOVE`, `DELETE`, `GET`) with custom headers and returns the response
  headers. Uploads do not follow redirects. Downloads go through memory.
- **Checkpoint's HTTP layer does not verify TLS certificates.** Traffic is encrypted, but a hostile
  network could impersonate the server. The Koofr app password limits the damage (revocable).
- **Sealed storage.** `device_seal` / `device_unseal` encrypt a blob with AES-256-GCM under a key
  from console-unique material, optionally plus a passphrase (the bundled `webdav` script keeps its
  `{"url","user","password"}` this way in `config/webdav.vault`). Reading the SD card alone yields
  nothing; other homebrew on the same console can reproduce the console half of the key; only a
  passphrase is a real wall.

**Why not the bundled restore.** `webdav.c`'s "Restore from server" caps both the server's list of
game folders and each game's list of zips at 128 (`MAX_PICK`), in the server's order, so with enough
games or versions some games and the newest versions are never offered. It finds a game by its
(sanitised, localised) name, so a renamed game or a changed console language loses the old history,
and an account save and a device save of the same game would share one folder.

**Koofr** (from 3DSaveSync's research and probe, 2026-11-12).
- WebDAV endpoint `https://app.koofr.net/dav/Koofr`, HTTP Basic with the account email and an **app
  password** (the account password gets 401). TLS 1.2 or newer.
- MKCOL new → 201, existing → 405. PUT → 201. GET → 200. PROPFIND Depth 1 → 207, missing → 404.
  MOVE with `Overwrite: F` → 201, onto an existing name → 412. DELETE → 204, missing → 404. PROPFIND
  hrefs start with `/dav/Koofr` and use the `D:` prefix.

**The console's clock.** A banned console cannot sync its time with Nintendo's servers, so its clock
can drift or reset.

## How it works

### Settings (owner's request: set up on the console, no PC needed)

- **Settings…** is an entry in the script's first dialog (see "Profiles").
- The owner types only the Koofr **email** and **app password** on the console keyboard. The server
  URL is fixed in the script: `https://app.koofr.net/dav/Koofr` (designer's call; Koofr is the only
  server).
- The saved email is shown in Settings and on the status line at the start of every run, so a typo
  is visible. The password is never shown.
- After entering them the script tests the connection at once (PROPFIND on the base URL) and says in
  words what is wrong: 401 → email or app password; 404/405 → address; transfer error → network;
  207 → works. Settings also has "Test connection" on its own.
- They are kept sealed in the script's **own** vault, `config/savesync.vault` (`device_seal`, no
  passphrase), independent of the bundled `webdav` script (designer's call). Reading the SD card
  alone yields nothing.
- First run with no vault → straight to Settings.

### Cloud layout

Under `<base URL>/SaveSyncNx/` (designer's call, like 3DSaveSync's `/3DSaveSync`; separate from the
bundled script's `Checkpoint/` folder):

```
SaveSyncNx/saves/0x<TITLEID> <name>/<version>.zip    account saves
SaveSyncNx/device/0x<TITLEID> <name>/<version>.zip   device saves
SaveSyncNx/.incoming/<unique>.zip                    uploads in progress
```

- Game folders are named like Checkpoint's own (`0x%016llX` + space + the name Checkpoint uses).
  For a title with any non-ASCII byte Checkpoint uses the id as the name (`0x<ID> 0x<ID>`); its cloud
  folder takes the real title name instead, with `/ \ : * ? " < > |` and control characters made `_`
  (owner, 2026-11-13). Restore identifies a game by the `0x<TITLEID>` part only; the name is for
  people browsing Koofr.
  If a game's name changes, new versions go to a new folder with the same id, and Restore shows the
  versions of both folders as one game.
- **Version name**: `YYYYMMDD-HHMMSS` in UTC, from the `Date` header of a Koofr response, not the
  console's clock (designer's call: the clock of a banned console cannot be trusted, and the names
  must sort in time order). Account saves add a space and the profile name:
  `20261112-184501 Serhii.zip`. Device saves have the bare timestamp.
- The zip is what `zip_dir` makes of the save's files, i.e. exactly what a Checkpoint backup folder
  holds, so unzipping it gives a folder Checkpoint can restore.

### Profiles

- The first dialog lists the profile names used before, then "New name…", "Restore…",
  "Settings…" and "Update script…". Picking a name starts the backup at once; "Restore…" goes to Restore. When there are
  no names yet (after Settings on the first run), it goes to the keyboard for the first name.
- A profile name may hold Latin letters, digits, space, `-` and `_` (it goes into file names on the
  card and the server); anything else, or a name equal to an existing one ignoring case, is refused
  with a message (designer's call).
- Names are kept in a small, non-secret config file on SD.
- Picking the wrong name labels that run's account saves with the other person's name, permanently
  (nothing is deleted automatically); the owner fixes it by hand in Koofr. Device saves are not
  affected (designer's call: no extra check dialog, which would cost the "no further input" goal).

### Backup

1. Open the vault; make sure `SaveSyncNx/`, `saves/`, `device/` and `.incoming/` exist (MKCOL; 405 =
   already there). Take the time for this run's version names from the `Date` header of these
   responses.
2. Clean up the script's own unfinished uploads: delete everything in `SaveSyncNx/.incoming/`.
3. For each catalog entry that is an account save or a device save, one at a time:
   1. Copy the save into a temporary folder on SD: walk the archive with `sav_list`, `sav_read` each
      file, `fwrite` it. Close the handle.
   2. `zip_dir` the folder into a temporary zip. Delete the folder.
   3. **Fingerprint** the zip from its central directory: the set of (path, size, CRC32) of every
      file. Read only the zip's tail; no per-byte work in script code.
   4. Compare with the fingerprint stored for this save. Equal → "unchanged", delete the zip, next.
   5. Different (or nothing stored) → upload (below). On success store the new fingerprint and write
      the state file **right away**, so an aborted run keeps what it finished. Delete the zip.
4. Summary: uploaded / unchanged / failed, also written to Checkpoint's log.

The state key identifies the save, not the run: profile name + title id for account saves; `device`
+ title id for device saves (device saves appear in every profile's catalog, so the second profile's
run sees them as unchanged).

Free space needed on SD during a run: about twice the largest single save.

### Upload

A version is uploaded under a name that does not exist yet and becomes visible only when complete:

1. PUT the zip to `SaveSyncNx/.incoming/<unique name>.zip`. Then PROPFIND (Depth 0) that file: if
   the `getcontentlength` Koofr reports is not the local zip's size, DELETE it and fail the save
   (owner, 2026-11-13; 3DSaveSync found Koofr answering a PUT 201 and serving `*.bin` back empty).
2. MOVE it to `SaveSyncNx/<saves|device>/0x<TITLEID> <name>/<version>.zip` with `Overwrite: F`
   (creating the game folder first if needed). **This is the completion point.** 412 (name taken) →
   the next free name (`<version> (2)`, `(3)`, …) and MOVE again. Never overwrite.
3. The cleanup at the start of each run deletes whatever is left in `.incoming/`: the only automatic
   deletion, and only of the script's own unfinished uploads (the rule the owner approved for
   3DSaveSync).

### Restore

1. List the game folders under `saves/` and `device/` (PROPFIND, no cap). Group them by title id.
   Show the games that the current catalog has a save for, of the same kind; label device saves as
   such. Games in the cloud with no save on the console are left out, with one line saying how many
   and that a game must be started once to create its save (designer's call).
2. Pick a game → its versions, newest first (sorted by name, which is UTC time), each showing its
   profile name.
3. Confirm → download the zip to a temporary file → `unzip` it (CRCs checked) into a temporary
   folder on SD, then rename that folder to `title_backup_path(idx, kind)/<version name>`; if that
   folder exists, `<version name> (2)`, `(3)`, …. Never write into an existing folder.
4. On any failure (network, CRC, cancel) nothing reaches Checkpoint's folder, so Checkpoint never
   offers a half-written backup: hold-B stops the script at the next statement, so only the rename
   after a whole unzip may put the folder in place (found in the final review). Delete the temporary
   zip and folder; whatever hold-B leaves of them goes at the next run's start.
5. Tell the owner the version is now in Checkpoint's backup list for that game, to be written into
   the game with Checkpoint's Restore (with the right profile selected for an account save).
6. B goes back one step: from the confirm to the versions, from the versions to the games; from the
   games it ends the run (designer's call).

### Update

1. "Update script..." asks GitHub for the latest release of `NuclleaR/SaveSyncNx`
   (`GET https://api.github.com/repos/NuclleaR/SaveSyncNx/releases/latest`, with a `User-Agent`
   header, which GitHub requires) and reads its `tag_name`.
2. The script knows its own version (`SAVESYNC_VERSION`, equal to the release tag). Same tag → "is
   the latest version". Otherwise it shows the current and the new version and asks to confirm.
3. On confirm it downloads `https://raw.githubusercontent.com/NuclleaR/SaveSyncNx/<tag>/scripts/savesync.c`
   to a temporary file, checks that it is a SaveSync script of that version (its
   `SAVESYNC_VERSION` line names the tag), and only then renames it over
   `<app root>/scripts/universal/savesync.c`. The new version runs from the next start.
4. Any failure leaves the installed script as it was.

The script must keep the name `savesync.c` in `scripts/universal/`; a copy under another name is not
the one that gets updated (designer's call, said in the README).

Risk the owner accepted: Checkpoint does not verify HTTPS certificates, so on a hostile network a
forged script could be served, and a script can read the Koofr login and the saves. Updates happen
only when the owner presses Update, and only from releases the owner published.

### Errors

- No network, login rejected (401/403), storage full (507): stop the run with a plain message, in the
  spirit of 3DSaveSync's texts. In the middle of a run, a transfer error or a 401/403 first gets one
  PROPFIND (Depth 0) of `SaveSyncNx/`: if Koofr answers it (207), that save alone fails and the run
  goes on; otherwise the run stops with the reason. A 507 always stops at once (owner, 2026-11-13:
  curl gives up below 300 B/s for 10 s, so one slow file would otherwise stop every run at the same
  game).
- If the cloud folders cannot be made at the start for any other reason, the run stops with the HTTP
  status Koofr answered (designer's call).
- Anything else fails only that save; the run goes on, the save is counted as failed, its stored
  fingerprint is left unchanged (so the next run retries it).
- Hold B (Checkpoint's abort) at any moment leaves: nothing changed in any save; no half-written
  folder in Checkpoint's backup list; at most one unfinished upload in `.incoming/` (deleted next
  run); temporary files on SD (deleted next run).

## Open items

1. **Saves of fully deleted games** (owner checks on the console later): does Checkpoint 5.3.0 list
   them? If not, a Checkpoint script cannot reach them, and the owner decides what to do then.

## The designer's calls (not discussed with the owner)

- Change detection by per-file CRC32 + size + path from the zip that `zip_dir` writes natively. A
  real change that keeps the same size and the same CRC32 of every changed file would be missed
  (odds about 1 in 4 billion per changed file); the next real change uploads it.
- Upload through `.incoming/` + MOVE with `Overwrite: F`, as in 3DSaveSync.
- Cloud root `SaveSyncNx/`, game folders named like Checkpoint's, games matched by title id.
- Version names in UTC from Koofr's `Date` header.
- Profile names limited to Latin letters, digits, space, `-`, `_`; no check against picking the
  wrong name.
- "Restore…" and "Settings…" live in the first dialog, so a backup still takes one pick.
- Own vault `config/savesync.vault`, without a passphrase (the 3DSaveSync threat model: the SD card
  alone must reveal nothing); Koofr URL fixed in the script.
- Script file `savesync.c` (menu entry "savesync"); UI text in English, as in 3DSaveSync.
- License GPL-3.0-or-later, like 3DSaveSync: the script will reuse code from Checkpoint's GPLv3
  `webdav.c` (Base64, WebDAV calls, vault reading), which requires it. Code adapted from it keeps
  Checkpoint's notice and says how it was modified.
- Not designed or tested for the 3DS.

## Left to the builder

- Format of the state and profile files, and where on SD they live (under `app_root()`).
- How the fingerprint is stored (the full list or a digest of it).
- Retries and timeouts.
- The test harness details (the owner chose the approach; see the plan): Checkpoint's
  `tools/scriptlint.sh` (parse check), a PC runner around Checkpoint's real interpreter, a fake
  Koofr, then the console.
