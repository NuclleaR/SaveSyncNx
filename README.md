# SaveSyncNx

A [Checkpoint](https://github.com/BernardoGiordano/Checkpoint) script for the Nintendo Switch that
backs up the save of every game to [Koofr](https://koofr.eu) over WebDAV in one action. It uploads
a save only when it changed, keeps every version in the cloud, leaves nothing on the console, and
downloads any version back into Checkpoint's backup folder so Checkpoint can restore it.

## Status

Work in progress. See the [design](docs/superpowers/specs/2026-11-12-savesyncnx-design.md) and the
[implementation plan](docs/superpowers/plans/2026-11-12-savesyncnx.md) for progress and every
deviation from the plan.

## License

GPL-3.0-or-later; see [LICENSE](LICENSE). Parts of `scripts/savesync.c` are adapted from
Checkpoint's `scripts/common/universal/webdav.c` (Copyright (C) 2017-2026 Bernardo Giordano,
FlagBrew, GPLv3 or later with Additional Terms 7.b and 7.c); the file keeps that notice and says how
it was modified.
