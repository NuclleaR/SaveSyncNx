# SaveSyncNx

A [Checkpoint](https://github.com/BernardoGiordano/Checkpoint) script for the Nintendo Switch that
backs up the save of every game to [Koofr](https://koofr.eu) over WebDAV in one action. It uploads
a save only when it changed, keeps every version in the cloud, leaves nothing on the console, and
downloads any version back into Checkpoint's backup folder so Checkpoint can restore it.

## Requirements

- A Switch with custom firmware and Checkpoint **5.3.0 or newer** (the script uses its scripting API).
- Wi-Fi.
- A Koofr account and a Koofr **app password** (Koofr: Preferences → Password → App passwords). The
  account password does not work over WebDAV.

## Install

Copy `scripts/savesync.c` to `sdmc:/switch/Checkpoint/scripts/universal/` on the SD card, over
Checkpoint's USB (MTP) or FTP. Keep the name `savesync.c`: "Update script..." replaces that file
and only that file, so a copy under another name is never updated.

## Usage

Start it in Checkpoint: Minus → Scripts → savesync.

**First run.** The script asks for the Koofr email and the app password on the console's keyboard,
then for a profile name (1–24 Latin letters, digits, spaces, `-` or `_`; the name goes into the
cloud file names). It then backs up every changed save and ends with
`Backup done: <u> uploaded, <n> unchanged, <f> failed.` `Restore...` is in the first dialog from
the second run on.

**Every later run.** The first dialog lists the profile names used before. Pick yours and the
backup runs with no further input. The same dialog has `New name...`, `Restore...`, `Settings...`
(change the email and app password, or test the connection) and `Update script...`.

**A second person.** Checkpoint shows the saves of the user selected in Checkpoint (switch it with
ZL). Select the other user, start savesync and pick or enter that person's name. Device saves are
shared by all users and are uploaded once, whichever profile is backing up.

**Restore.** `Restore...` → a game → a version (newest first; account versions show the profile
name) → confirm. The version is downloaded into a new folder in Checkpoint's backup list for that
game; nothing is written into the game. Then pick that backup in Checkpoint and press Restore, with
the right user selected for an account save. B goes back one step.

**Stopping.** Hold B to abort, as with any Checkpoint script. No save is ever changed, and a
restore stopped half way leaves no folder in Checkpoint's backup list (the version is unpacked in
`savesync/` and moved into place only when complete). An unfinished upload and the temporary files
are deleted at the start of the next run.

**When something fails.** No connection, a rejected login or a full Koofr storage stop the run with
a message. If one save's upload breaks off or is refused while Koofr still answers, only that save
counts as failed; the run goes on and the next run retries it.

## What is stored where

In Koofr, under `SaveSyncNx/`:

| Path | What |
|---|---|
| `saves/0x<TITLEID> <game>/<YYYYMMDD-HHMMSS> <profile>.zip` | an account save of that profile |
| `device/0x<TITLEID> <game>/<YYYYMMDD-HHMMSS>.zip` | a device save |
| `.incoming/` | uploads in progress; whatever is left there is deleted by the next run |

`<game>` is the name of Checkpoint's backup folder for that game. Checkpoint names the folder of a
title with non-Latin characters `0x<TITLEID> 0x<TITLEID>`; in Koofr that game's folder has the real
title name instead, so it can be found by hand. Restore matches games by the title id only.

The time is UTC, taken from Koofr's clock. A name that is taken gets ` (2)`, ` (3)`, …: no version
is ever overwritten or deleted. Each zip holds exactly what a Checkpoint backup folder holds.

On the SD card, under `sdmc:/switch/Checkpoint/`:

| Path | What |
|---|---|
| `config/savesync.vault` | the Koofr email and app password, sealed with this console's key (never in plain text) |
| `config/savesync-profiles.txt` | the profile names, one per line |
| `config/savesync-state.txt` | the fingerprint of each save as last uploaded, to tell what changed |
| `savesync/` | temporary files of a run; emptied at the start of every run |

A run needs free space on the SD card of about twice the largest single save.

## Limitations

- Only the games in Checkpoint's list for the selected user are backed up. BCAT and system saves are
  skipped.
- Whether Checkpoint 5.3.0 lists the saves of games that were fully deleted is not yet checked
  (open item in the design). If it does not, a script cannot reach them.
- Checkpoint does not verify HTTPS certificates. On a hostile network a forged update could be
  served; updates happen only when you press "Update script..." and confirm.
- After each upload the script checks that Koofr holds the full size of the zip, and fails that
  save otherwise. It does not download the zip again to compare the bytes.
- On a new or replaced console, the first run backs up the saves that are there before `Restore...`
  can be picked, so those versions become the newest in Koofr. The older versions are still there;
  pick one in Restore.
- Picking the wrong profile name labels that run's account saves with the other name, permanently;
  rename or move the files in Koofr by hand. Device saves are not affected.

## Updating

`Update script...` asks GitHub for the latest release, shows the current and the new version, and
on confirm downloads that release's `scripts/savesync.c`. It replaces the installed script only
after checking that the download is SaveSync of that version. The new version runs from the next
start.

On the Switch's SD card a rename cannot replace a file, so the update first moves the installed
script to `sdmc:/switch/Checkpoint/savesync/installed.old` and puts it back if the new one cannot
take its place. If an update ever leaves no `savesync.c` in `scripts/universal/`, copy
`savesync/installed.old` there and name it `savesync.c`. Do it before you start any other copy of
the script: every run empties `savesync/`.

**Releasing a new version:**

1. Set `#define SAVESYNC_VERSION` in `scripts/savesync.c` to the new tag, e.g. `"v0.2.0"`.
2. Run `make test`. Release only when it passes.
3. Commit, tag `v0.2.0` and push the tag.
4. Publish a GitHub release for that tag (not a draft or a pre-release, which `releases/latest`
   does not return).

The repository must stay public: the console downloads the script without logging in.

## Development

The tests run the script on the PC in Checkpoint 5.3.0's real picoc interpreter and script
bindings (`tools/hostrun`, a fake console) against a fake Koofr (`tests/fakedav.py`).

On Arch Linux:

```sh
sudo pacman -S base-devel curl mbedtls3
mise trust && mise install          # pytest, from mise.toml
git submodule update --init --recursive
make test                           # lint + build/hostrun + pytest
```

`make lint` checks the script with Checkpoint's own parser. Three hostrun flags give the tests the
conditions a fake server cannot make: `--abort-during unzip` (or `zip`) presses hold-B while that
binding runs, `--fail-close <path part>` makes `fclose` fail for matching files, as a full SD card
would, and `--fat-rename` makes `rename` refuse an existing destination, as the Switch's FAT card
does. The design is in
[docs/superpowers/specs/2026-11-12-savesyncnx-design.md](docs/superpowers/specs/2026-11-12-savesyncnx-design.md),
the implementation plan and every deviation from it in
[docs/superpowers/plans/2026-11-12-savesyncnx.md](docs/superpowers/plans/2026-11-12-savesyncnx.md),
and the checks to make on a real console in [docs/console-check.md](docs/console-check.md).

## License

GPL-3.0-or-later; see [LICENSE](LICENSE). Parts of `scripts/savesync.c` are adapted from
Checkpoint's `scripts/common/universal/webdav.c` (Copyright (C) 2017-2026 Bernardo Giordano,
FlagBrew, GPLv3 or later with Additional Terms 7.b and 7.c); the file keeps that notice and says how
it was modified.
