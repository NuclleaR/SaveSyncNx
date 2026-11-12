/*
 * The transcript and status taps.
 *
 * Every byte a script prints reaches Checkpoint's ScriptConsole through one of
 * three doors, and each is tapped here:
 *   - printf and friends: the stream from ckpt_console_stdout(), built with
 *     funopen; hostrun's funopen shim (shim/platform.cpp) calls Capture::output;
 *   - picoc's putc path and its diagnostics: ckpt_console_write(), wrapped with
 *     the linker's --wrap;
 *   - script_log: ScriptConsole::log(), wrapped the same way.
 * gui_status reaches ScriptUiBridge::setStatus(), also wrapped. --wrap only
 * redirects calls from other object files, which is exactly the set above:
 * ScriptConsole's own internal calls stay unwrapped, so nothing is counted twice.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "capture.hpp"
#include "scriptconsole.hpp"
#include "scriptuibridge.hpp"
#include <mutex>

namespace {
    std::mutex g_mutex;
    std::string g_transcript;
    std::vector<std::string> g_statuses;
}

void Capture::output(const char* data, size_t len)
{
    if (data == nullptr || len == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_transcript.append(data, len);
}

void Capture::status(const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_statuses.push_back(text);
}

std::string Capture::transcript(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_transcript;
}

std::vector<std::string> Capture::statuses(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_statuses;
}

/* ---- linker wraps (see the Makefile's --wrap list) ---------------------- */

extern "C" {

void __real_ckpt_console_write(const char* data, int len);

void __wrap_ckpt_console_write(const char* data, int len)
{
    if (len > 0) {
        Capture::output(data, (size_t)len);
    }
    __real_ckpt_console_write(data, len);
}

// void ScriptConsole::log(const std::string&)
void __real__ZN13ScriptConsole3logERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(ScriptConsole* self, const std::string& text);

void __wrap__ZN13ScriptConsole3logERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(ScriptConsole* self, const std::string& text)
{
    Capture::output(text.data(), text.size());
    Capture::output("\n", 1);
    __real__ZN13ScriptConsole3logERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(self, text);
}

// void ScriptUiBridge::setStatus(std::string). A by-value class argument is
// passed by address under the Itanium ABI and destroyed by the caller, so the
// wrapper hands the same address on.
void __real__ZN14ScriptUiBridge9setStatusENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(ScriptUiBridge* self, std::string* text);

void __wrap__ZN14ScriptUiBridge9setStatusENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(ScriptUiBridge* self, std::string* text)
{
    Capture::status(*text);
    __real__ZN14ScriptUiBridge9setStatusENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE(self, text);
}
}
