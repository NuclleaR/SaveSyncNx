/*
 * hostrun shim for Checkpoint's switch/include/thread.hpp (libnx threads).
 * Same two calls, backed by pthreads on the PC.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef THREAD_HPP
#define THREAD_HPP

#include <cstddef>
#include <optional>

namespace Threads {
    // One worker slot, like the console: create() reaps the previous worker
    // first. The stack size is honoured, so a script that would overflow the
    // console's 128 KB worker stack fails here too.
    bool create(std::optional<size_t> stackSize, void (*entrypoint)(void));
    void join(void);
}

#endif
