/*
 * What hostrun records from a run besides the exit value: the full transcript
 * (Checkpoint's own console keeps only the last 250 wrapped lines) and every
 * gui_status text in order.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HOSTRUN_CAPTURE_HPP
#define HOSTRUN_CAPTURE_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace Capture {
    // Script thread. Raw bytes as the script printed them.
    void output(const char* data, size_t len);
    void status(const std::string& text);

    // Main thread, after the run.
    std::string transcript(void);
    std::vector<std::string> statuses(void);
}

#endif
