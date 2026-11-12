/*
 * hostrun shim for Checkpoint's switch/include/paths.hpp, which includes
 * <switch.h>. The bindings hostrun links use only Paths::checkpointRoot().
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PATHS_HPP
#define PATHS_HPP

namespace Paths {
    // "sdmc:/switch/Checkpoint", as on the console. hostrun runs with the fake
    // SD card as its working directory, which holds a real directory named
    // "sdmc:", so the same string resolves on the PC.
    const char* checkpointRoot(void);
}

#endif
