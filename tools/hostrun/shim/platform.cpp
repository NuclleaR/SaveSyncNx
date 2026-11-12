/*
 * hostrun's stand-ins for the platform pieces the linked Checkpoint sources
 * need: Paths::checkpointRoot, Threads, Logging (common/logging.cpp is
 * libctru/libnx only) and newlib's funopen.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../capture.hpp"
#include "funopen.h"
#include "logging.hpp"
#include "paths.hpp"
#include "thread.hpp"
#include <algorithm>
#include <climits>
#include <cstdio>
#include <pthread.h>
#include <sys/types.h>

/* ---- Paths -------------------------------------------------------------- */

const char* Paths::checkpointRoot(void)
{
    return "sdmc:/switch/Checkpoint";
}

/* ---- Threads ------------------------------------------------------------ */

namespace {
    pthread_t g_thread;
    bool g_live                  = false;
    void (*g_entrypoint)(void)   = nullptr;

    void* trampoline(void*)
    {
        g_entrypoint();
        return nullptr;
    }
}

bool Threads::create(std::optional<size_t> stackSize, void (*entrypoint)(void))
{
    join();
    g_entrypoint = entrypoint;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stackSize) {
        pthread_attr_setstacksize(&attr, std::max<size_t>(*stackSize, PTHREAD_STACK_MIN));
    }
    g_live = pthread_create(&g_thread, &attr, trampoline, nullptr) == 0;
    pthread_attr_destroy(&attr);
    return g_live;
}

void Threads::join(void)
{
    if (g_live) {
        pthread_join(g_thread, nullptr);
        g_live = false;
    }
}

/* ---- Logging ------------------------------------------------------------ */
/* Warnings and errors go to stderr, which the tests append to the output.
 * Info and below are dropped: the run's own lines already reach the
 * transcript, and the engine's "output:" echo would duplicate it. */

void Logging::init(void) {}
void Logging::initFileLogging(void) {}
void Logging::exit(void) {}
void Logging::installCrashHandlers(void) {}

void Logging::log(LogLevel level, const std::string& message)
{
    if (level == LogLevel::WARN) {
        fprintf(stderr, "warning: %s\n", message.c_str());
    }
    else if (level == LogLevel::ERROR) {
        fprintf(stderr, "error: %s\n", message.c_str());
    }
}

std::string Logging::getApplicationLogs(void)
{
    return "";
}

void Logging::trace(const std::string& message)
{
    log(LogLevel::TRACE, message);
}

void Logging::debug(const std::string& message)
{
    log(LogLevel::DEBUG, message);
}

void Logging::info(const std::string& message)
{
    log(LogLevel::INFO, message);
}

void Logging::warning(const std::string& message)
{
    log(LogLevel::WARN, message);
}

void Logging::error(const std::string& message)
{
    log(LogLevel::ERROR, message);
}

/* ---- funopen ------------------------------------------------------------ */

namespace {
    struct Cookie {
        void* user;
        int (*write)(void*, const char*, int);
    };

    ssize_t cookieWrite(void* cookie, const char* buf, size_t size)
    {
        Cookie* c = static_cast<Cookie*>(cookie);
        Capture::output(buf, size);
        const int n = c->write(c->user, buf, (int)size);
        return n < 0 ? -1 : (ssize_t)n;
    }
}

extern "C" FILE* hostrun_funopen(const void* cookie, void*, int (*writefn)(void*, const char*, int), void*, void*)
{
    // Never freed: like the console's, the stream lives for the process.
    Cookie* c                 = new Cookie{const_cast<void*>(cookie), writefn};
    cookie_io_functions_t io = {nullptr, cookieWrite, nullptr, nullptr};
    return fopencookie(c, "w", io);
}
