/*
 * hostrun: run a Checkpoint script on the PC, in Checkpoint's own interpreter
 * and bindings, against a fake console.
 *
 *   hostrun --catalog <catalog.json> --answers <answers.json> --result <result.json>
 *           [--abort-during <zip|unzip>] [--fail-close <path part>] [--fat-rename] <script.c>
 *
 * --abort-during presses hold-B while that binding runs; --fail-close makes
 * fclose fail for files whose path contains the part; --fat-rename makes
 * rename refuse an existing destination, as FAT does (see faults.hpp).
 *
 * The working directory is the fake SD card: it holds a real directory named
 * "sdmc:", so "sdmc:/switch/Checkpoint/..." resolves inside it. The script runs
 * on ScriptRunner's worker thread exactly as on the console; this thread plays
 * the console's main loop and answers each dialog from answers.json in order.
 *
 * Exit code: the script's exit value, or 3 for a runner error (a dialog with no
 * answer left, an answer of the wrong kind, bad arguments).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "capture.hpp"
#include "fakehost.hpp"
#include "faults.hpp"
#include "json.hpp"
#include "scriptrunner.hpp"
#include "thread.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

using nlohmann::json;

namespace {
    constexpr int RUNNER_ERROR = 3;

    const char* kindName(UiRequest::Kind kind)
    {
        switch (kind) {
            case UiRequest::Kind::Message:
                return "Message";
            case UiRequest::Kind::Confirm:
                return "Confirm";
            case UiRequest::Kind::PickOne:
                return "PickOne";
            case UiRequest::Kind::PickMany:
                return "PickMany";
            case UiRequest::Kind::Keyboard:
                return "Keyboard";
            case UiRequest::Kind::Numpad:
                return "Numpad";
        }
        return "?";
    }

    bool readJson(const std::string& path, json& out, std::string& error)
    {
        std::ifstream in(path);
        if (!in) {
            error = "cannot read " + path;
            return false;
        }
        out = json::parse(in, nullptr, false);
        if (out.is_discarded()) {
            error = path + ": not valid JSON";
            return false;
        }
        return true;
    }

    // Builds the response to `req` from `answer`; false with `error` set when
    // the answer does not fit the dialog.
    bool answerFor(const UiRequest& req, const json& answer, UiResponse& resp, std::string& error)
    {
        const std::string want = kindName(req.kind);
        const std::string got  = answer.value("kind", std::string());
        if (got != want) {
            error = "expected an answer for " + want + " '" + req.prompt + "', the next answer is " + got;
            return false;
        }
        switch (req.kind) {
            case UiRequest::Kind::Message:
                return true;
            case UiRequest::Kind::Confirm:
                resp.confirmed = answer.value("yes", false);
                return true;
            case UiRequest::Kind::PickOne:
                if (answer.contains("item")) {
                    const std::string item = answer["item"].get<std::string>();
                    for (size_t i = 0; i < req.items.size(); i++) {
                        if (req.items[i] == item) {
                            resp.index = (int)i;
                            return true;
                        }
                    }
                    error = "PickOne '" + req.prompt + "' has no item '" + item + "'";
                    return false;
                }
                resp.index = answer.value("index", -1);
                return true;
            case UiRequest::Kind::Keyboard:
                resp.text = answer.value("text", std::string());
                return true;
            default:
                error = want + " dialogs are not supported by hostrun";
                return false;
        }
    }
}

int main(int argc, char** argv)
{
    std::string catalogPath, answersPath, resultPath, scriptPath;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if ((arg == "--catalog" || arg == "--answers" || arg == "--result") && i + 1 < argc) {
            (arg == "--catalog" ? catalogPath : arg == "--answers" ? answersPath : resultPath) = argv[++i];
        }
        else if (arg == "--abort-during" && i + 1 < argc) {
            Faults::abortDuring(argv[++i]);
        }
        else if (arg == "--fail-close" && i + 1 < argc) {
            Faults::failClose(argv[++i]);
        }
        else if (arg == "--fat-rename") {
            Faults::fatRename();
        }
        else if (scriptPath.empty() && arg.rfind("--", 0) != 0) {
            scriptPath = arg;
        }
        else {
            scriptPath.clear();
            break;
        }
    }
    if (catalogPath.empty() || answersPath.empty() || resultPath.empty() || scriptPath.empty()) {
        fprintf(stderr, "usage: hostrun --catalog <catalog.json> --answers <answers.json> --result <result.json>\n"
                        "               [--abort-during <zip|unzip>] [--fail-close <path part>] [--fat-rename] <script.c>\n");
        return RUNNER_ERROR;
    }

    std::string error;
    json answers;
    if (!FakeHost::instance().load(catalogPath, error) || !readJson(answersPath, answers, error) || !answers.is_array()) {
        fprintf(stderr, "hostrun: %s\n", error.empty() ? "answers must be a JSON list" : error.c_str());
        return RUNNER_ERROR;
    }

    ScriptRunner& runner = ScriptRunner::get();
    if (!runner.start(scriptPath, scriptPath, "")) {
        fprintf(stderr, "hostrun: the script worker did not start\n");
        return RUNNER_ERROR;
    }

    json ui            = json::array();
    size_t nextAnswer  = 0;
    std::string runnerError;
    ScriptRunner::Outcome outcome;
    while (true) {
        if (auto done = runner.takeResult()) {
            outcome = *done;
            break;
        }
        std::optional<UiRequest> req = runner.bridge().take();
        if (!req) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        ui.push_back({{"kind", kindName(req->kind)}, {"prompt", req->prompt}, {"items", req->items}});
        UiResponse resp;
        if (!runnerError.empty()) {
            runner.requestCancel();
        }
        else if (nextAnswer >= answers.size()) {
            runnerError = std::string("no answer left for ") + kindName(req->kind) + " '" + req->prompt + "'";
            runner.requestCancel();
        }
        else if (!answerFor(*req, answers[nextAnswer++], resp, runnerError)) {
            runner.requestCancel();
        }
        else {
            runner.bridge().respond(resp);
        }
    }
    Threads::join();

    std::string output = Capture::transcript();
    if (!runnerError.empty()) {
        output += "\nhostrun: " + runnerError + "\n";
    }
    const int exitValue = runnerError.empty() ? outcome.exitValue : RUNNER_ERROR;

    json result = {
        {"exit", exitValue},
        {"cancelled", outcome.cancelled},
        {"output", output},
        {"ui", ui},
        {"status", Capture::statuses()},
        {"saveWrites", FakeHost::instance().saveWrites()},
    };
    std::ofstream out(resultPath);
    out << result.dump(1, ' ', false, json::error_handler_t::replace) << "\n";
    return exitValue;
}
