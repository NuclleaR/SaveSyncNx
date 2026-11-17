/*
 * Faults a test can switch on from the command line, for failures a fake
 * server cannot cause: hold-B in the middle of a binding, and an SD card that
 * fails the final flush of a file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <string>

namespace Faults {
    // Raise Checkpoint's abort (hold-B) on the first progress tick of the
    // binding that opens its byte bar with `label` ("unzip", "zip").
    void abortDuring(const std::string& label);

    // fclose returns EOF for every file whose path contains `part`.
    void failClose(const std::string& part);

    // rename refuses an existing destination, as the Switch's FAT SD card
    // does (on the PC it would replace it).
    void fatRename(void);
}
