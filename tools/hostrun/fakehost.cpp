/*
 * FakeHost, modelled on Checkpoint's switch/source/script/scripthost.cpp: same
 * handle table size, same backup folder names, same answers for the kinds the
 * Switch does not have.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fakehost.hpp"
#include "common.hpp"
#include "json.hpp"
#include "scriptheap.hpp"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>

FakeHost& FakeHost::instance(void)
{
    static FakeHost host;
    return host;
}

ScriptHost& ScriptHost::get(void)
{
    return FakeHost::instance();
}

bool FakeHost::load(const std::string& catalogPath, std::string& error)
{
    std::ifstream in(catalogPath);
    if (!in) {
        error = "cannot read " + catalogPath;
        return false;
    }
    nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.contains("titles") || !doc["titles"].is_array()) {
        error = catalogPath + ": expected {\"titles\": [...]}";
        return false;
    }
    for (const auto& t : doc["titles"]) {
        FakeTitle title;
        title.id   = std::stoull(t.value("id", std::string("0")), nullptr, 16);
        title.name = t.value("name", std::string());
        title.kind = t.value("kind", std::string("account"));
        title.dir  = t.value("dir", std::string());
        if (title.kind != "account" && title.kind != "device" && title.kind != "bcat" && title.kind != "system") {
            error = "unknown title kind '" + title.kind + "'";
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(backupPath(title), ec);
        if (ec) {
            error = "cannot create " + backupPath(title) + ": " + ec.message();
            return false;
        }
        mTitles.push_back(title);
    }
    return true;
}

std::vector<std::string> FakeHost::saveWrites(void)
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mWrites;
}

void FakeHost::record(std::string what)
{
    std::lock_guard<std::mutex> lock(mMutex);
    mWrites.push_back(std::move(what));
}

// switch/source/titleprobe.cpp + savedatasource.cpp: base folder per save kind,
// then "0x<ID> <name>", with the id standing in for a name it cannot use.
std::string FakeHost::backupPath(const FakeTitle& title) const
{
    std::string base = "saves";
    if (title.kind == "device") {
        base = "device";
    }
    else if (title.kind == "bcat") {
        base = "bcat";
    }
    else if (title.kind == "system") {
        base = "system";
    }
    const std::string id       = StringUtils::format("0x%016llX", (unsigned long long)title.id);
    const std::string safeName = StringUtils::containsInvalidChar(title.name) ? id : StringUtils::removeForbiddenCharacters(title.name);
    return "sdmc:/switch/Checkpoint/" + base + "/" + id + " " + safeName;
}

int FakeHost::titleCount(void)
{
    return (int)mTitles.size();
}

bool FakeHost::titleAt(int idx, HostTitle& out)
{
    if (idx < 0 || idx >= titleCount()) {
        return false;
    }
    out.id      = mTitles[idx].id;
    out.name    = mTitles[idx].name;
    out.hasSave = true; // every catalog entry on Switch is an installed save
    return true;
}

int FakeHost::titleIndexOf(uint64_t id)
{
    for (size_t i = 0; i < mTitles.size(); i++) {
        if (mTitles[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

std::string FakeHost::titleBackupPath(int idx, int kind)
{
    if (kind != 0 || idx < 0 || idx >= titleCount()) {
        return "";
    }
    return backupPath(mTitles[idx]);
}

int FakeHost::savOpen(int titleIdx, int kind)
{
    if (kind != 0 || titleIdx < 0 || titleIdx >= titleCount()) {
        return -1; // no extdata on Switch
    }
    if (mTitles[titleIdx].dir.empty()) {
        return -5; // stands in for a platform Result: the save would not mount
    }
    for (int i = 0; i < MAX_SAV_HANDLES; i++) {
        if (!mSlots[i].live) {
            mSlots[i].live = true;
            mSlots[i].dir  = mTitles[titleIdx].dir;
            return i;
        }
    }
    return -2;
}

bool FakeHost::savValid(int handle)
{
    return handle >= 0 && handle < MAX_SAV_HANDLES && mSlots[handle].live;
}

std::string FakeHost::pathIn(int handle, const std::string& path) const
{
    return mSlots[handle].dir + (path.empty() || path[0] != '/' ? "/" : "") + path;
}

int FakeHost::savRead(int handle, const char* path, char** outBuf, int* outSize)
{
    FILE* f = fopen(pathIn(handle, path).c_str(), "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    rewind(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    char* buf = (char*)ScriptHeap::get().alloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return -3;
    }
    const size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        ScriptHeap::get().release(buf);
        return -4;
    }
    buf[read] = '\0';
    *outBuf   = buf;
    *outSize  = (int)read;
    return 0;
}

int FakeHost::savWrite(int, const char* path, const void*, size_t)
{
    record(std::string("sav_write ") + path);
    return 0;
}

int FakeHost::savDelete(int, const char* path)
{
    record(std::string("sav_delete ") + path);
    return 0;
}

int FakeHost::savMkdir(int, const char* path)
{
    record(std::string("sav_mkdir ") + path);
    return 0;
}

int FakeHost::savRmdir(int, const char* path)
{
    record(std::string("sav_rmdir ") + path);
    return 0;
}

int FakeHost::savRename(int, const char* from, const char* to)
{
    record(std::string("sav_rename ") + from + " " + to);
    return 0;
}

bool FakeHost::savList(int handle, const std::string& dir, std::vector<HostDirEntry>& out)
{
    DIR* d = opendir(pathIn(handle, dir).c_str());
    if (!d) {
        return false;
    }
    while (struct dirent* ent = readdir(d)) {
        const std::string name = ent->d_name;
        if (name != "." && name != "..") {
            out.push_back({name, ent->d_type == DT_DIR});
        }
    }
    closedir(d);
    return true;
}

int FakeHost::savCommit(int)
{
    record("sav_commit");
    return 0;
}

void FakeHost::savClose(int handle)
{
    if (handle >= 0 && handle < MAX_SAV_HANDLES) {
        mSlots[handle] = Slot{};
    }
}

void FakeHost::savCloseAll(void)
{
    for (auto& slot : mSlots) {
        slot = Slot{};
    }
}

// Fixed bytes stand in for the console key, so a blob sealed in one run opens
// in the next, as it does on one console.
int FakeHost::deviceSecret(uint8_t* out, int want)
{
    if (want == DeviceKeySourceBest || want == DeviceKeySourceNxSpl) {
        for (int i = 0; i < 32; i++) {
            out[i] = (uint8_t)(0xA0 + i);
        }
        return DeviceKeySourceNxSpl;
    }
    if (want == DeviceKeySourceNxCal) {
        for (int i = 0; i < 32; i++) {
            out[i] = (uint8_t)(0x50 + i);
        }
        return DeviceKeySourceNxCal;
    }
    return -1;
}

bool FakeHost::randomBytes(void* out, size_t size)
{
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f) {
        return false;
    }
    const bool ok = fread(out, 1, size, f) == size;
    fclose(f);
    return ok;
}
