# Console check

What the PC tests cannot show: the real Switch, the real Checkpoint 5.3.0 and the real Koofr. Run
these in order on the console with `scripts/savesync.c` installed in
`sdmc:/switch/Checkpoint/scripts/universal/`. Write the result after each line, and anything that
differs into the plan's Deviations.

Start every run with Minus → Scripts → savesync.

## Login

- [ ] **First run, a wrong app password.** Enter the Koofr email and a wrong password, then a
  profile name.
  Expected: right after the password, the connection test says `Koofr rejected the email or app
  password (401).` with the app-password hint. After the profile name, the backup stops with
  `Koofr rejected the login. Check email and app password in Settings.`; nothing is uploaded.
- [ ] **Settings → Test connection** with the wrong password.
  Expected: `Koofr rejected the email or app password (401).` and the app-password hint.
- [ ] **Settings → Change email and app password**, the right ones, then **Test connection**.
  Expected: `Connected to Koofr as <email>.` The status line reads `Koofr: <email>`.
- [ ] Look at `sdmc:/switch/Checkpoint/config/savesync.vault` on a PC.
  Expected: no readable email or password in it.

## Backup

- [ ] **First backup.** Pick your profile name.
  Expected: one line per game in the log, then `Backup done: <u> uploaded, 0 unchanged, 0 failed.`
  In Koofr: `SaveSyncNx/saves/0x<ID> <game>/<time> <profile>.zip` per account save and
  `SaveSyncNx/device/…/<time>.zip` per device save; `.incoming/` is empty. The time is UTC.
- [ ] **Download one zip from Koofr on a PC and open it.**
  Expected: the save's files, the same as a Checkpoint backup folder of that game. (Koofr serves some
  `*.bin` files back empty; the script checks the stored size after each upload, and a zip that
  opens shows the bytes are right too.) The log has no `upload: Koofr holds … bytes` line.
- [ ] **A game whose name has non-Latin characters** (e.g. a Japanese title), if there is one.
  Expected: Checkpoint's own folder is `0x<ID> 0x<ID>`; the Koofr folder is `0x<ID> <real name>`.
- [ ] **Second run, nothing played.**
  Expected: `Backup done: 0 uploaded, <n> unchanged, 0 failed.`, no new files in Koofr.
- [ ] **Play one game until it saves, then run again.**
  Expected: `1 uploaded`; that game's folder has a second version, the old one is still there.
- [ ] **Hold B during an upload** (a game with a large save), then run again.
  Expected: the first run stops; at most one file in `.incoming/`. The second run empties
  `.incoming/` and uploads that save.
- [ ] Look at `sdmc:/switch/Checkpoint/savesync/` after a run.
  Expected: empty.
- [ ] **Wi-Fi off, run a backup.**
  Expected: `No connection to Koofr. Check Wi-Fi.`
- [ ] **Turn Wi-Fi off in the middle of a backup** (airplane mode while a large save uploads).
  Expected: the log says that save failed, then the run stops with `No connection to Koofr. Check
  Wi-Fi.` (one check of Koofr after the failure, not one per remaining game).

## The second profile

- [ ] **Switch to the son's user in Checkpoint with ZL, start savesync, `New name...`, enter his
  name.**
  Expected: his account saves upload under his name; device saves count as unchanged.
- [ ] **Run again with his name.**
  Expected: everything unchanged.

## Restore

- [ ] **`Restore...`.**
  Expected: the games of this console that have backups; device saves end in `(device)`; if Koofr
  has games this console has no save of, the prompt says how many.
- [ ] **Pick a game and a version, confirm.**
  Expected: `Downloaded. Pick it in Checkpoint's backup …`. Checkpoint's backup list for that game
  shows a folder named like the version.
- [ ] **Restore that backup with Checkpoint's own Restore and start the game.**
  Expected: the game has that save.
- [ ] **Restore the same version again.**
  Expected: a second folder, `<version> (2)`; the first is untouched.
- [ ] **Hold B while a large version is being unpacked** (after `Downloading from Koofr...`).
  Expected: no new folder in Checkpoint's backup list for that game. This also checks that the SD
  card renames a folder (`savesync/restore` → the backup folder), which the PC tests cannot show.
- [ ] **Restore a game whose name has `'`, `&`, `#` or `%`** (e.g. Luigi's Mansion 3), if there is one.
  Expected: the same as any other game.
- [ ] **A replaced console or a fresh install:** install the script, enter the login, enter a
  profile name.
  Expected: the first run backs up first (owner's decision); from the second run `Restore...` lists
  the older versions next to the new ones.

## Update

- [ ] **`Update script...` while the installed version is the latest release.**
  Expected: `SaveSync <version> is the latest version.`
- [ ] **After a newer release is published, `Update script...` and confirm.**
  Expected: the offer names both versions; then `Updated to <new>. Run savesync again to use it.`
  The next start shows `SaveSync <new>` in the first dialog. This also checks the FAT path of the
  rename (moving the old script aside) on a real card; the PC tests only simulate it.

## Open item 1

- [ ] **Delete one game's software but keep its save data** (System Settings → Data Management →
  Delete Software), then open Checkpoint and run a backup.
  Is the game still in Checkpoint's list, and does savesync back it up (`unchanged` or `uploaded`
  in the log)? Answer the design's Open item 1 with what you see.
