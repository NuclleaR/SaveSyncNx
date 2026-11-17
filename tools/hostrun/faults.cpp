/*
 * The fault taps (see faults.hpp), as linker wraps like capture.cpp's:
 *   - ScriptConsole::beginIo / addIo: zip_dir and unzip open the byte bar with
 *     their name and tick it per chunk, polling the abort flag in between, so
 *     raising the abort on a tick is hold-B pressed while the binding runs;
 *   - fopen / fclose: the files opened under a matching path report a failed
 *     close (after really closing), as a full or failing SD card would.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "faults.hpp"
#include "scriptconsole.hpp"
#include <cstdio>
#include <mutex>
#include <set>

extern "C" {
#include "checkpoint_api.h"
}

namespace {
    std::mutex g_mutex;
    std::string g_abortLabel;
    std::string g_currentIo;
    std::string g_closePart;
    std::set<FILE*> g_failing;
}

void Faults::abortDuring(const std::string& label)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_abortLabel = label;
}

void Faults::failClose(const std::string& part)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_closePart = part;
}

extern "C" {

// void ScriptConsole::beginIo(std::string, long long): the by-value string is
// passed by address (see capture.cpp's setStatus).
void __real__ZN13ScriptConsole7beginIoENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEx(ScriptConsole* self, std::string* label, long long total);

void __wrap__ZN13ScriptConsole7beginIoENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEx(ScriptConsole* self, std::string* label, long long total)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_currentIo = *label;
    }
    __real__ZN13ScriptConsole7beginIoENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEx(self, label, total);
}

// void ScriptConsole::addIo(long long)
void __real__ZN13ScriptConsole5addIoEx(ScriptConsole* self, long long delta);

void __wrap__ZN13ScriptConsole5addIoEx(ScriptConsole* self, long long delta)
{
    bool abort = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        abort = !g_abortLabel.empty() && g_currentIo == g_abortLabel;
    }
    if (abort) {
        ckpt_script_abort_request();
    }
    __real__ZN13ScriptConsole5addIoEx(self, delta);
}

FILE* __real_fopen(const char* path, const char* mode);

FILE* __wrap_fopen(const char* path, const char* mode)
{
    FILE* f = __real_fopen(path, mode);
    if (f != nullptr && path != nullptr) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_closePart.empty() && std::string(path).find(g_closePart) != std::string::npos) {
            g_failing.insert(f);
        }
    }
    return f;
}

int __real_fclose(FILE* f);

int __wrap_fclose(FILE* f)
{
    bool fail = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        fail = g_failing.erase(f) > 0;
    }
    const int rc = __real_fclose(f);
    return fail ? EOF : rc;
}
}
