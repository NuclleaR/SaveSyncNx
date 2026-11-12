/*
 * FakeHost: Checkpoint's ScriptHost seam backed by folders on the PC.
 *
 * The catalog comes from catalog.json; each title's save archive is a plain
 * folder ("dir") that sav_open exposes read-only. Write-type sav_* calls touch
 * nothing: they return 0 and are recorded, so a test can prove a script never
 * writes a save.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HOSTRUN_FAKEHOST_HPP
#define HOSTRUN_FAKEHOST_HPP

#include "scripthost.hpp"
#include <mutex>
#include <string>
#include <vector>

struct FakeTitle {
    uint64_t id = 0;
    std::string name;
    std::string kind; // account | device | bcat | system
    std::string dir;  // "" = the save cannot be opened (sav_open answers -5)
};

class FakeHost : public ScriptHost {
public:
    static FakeHost& instance(void);

    // Reads catalog.json and creates each title's backup folder, as Checkpoint
    // does when it lists the title. False with `error` set on a bad catalog.
    bool load(const std::string& catalogPath, std::string& error);

    // "sav_write /x", "sav_delete /x", "sav_mkdir /d", "sav_rmdir /d",
    // "sav_rename /a /b", "sav_commit", in call order.
    std::vector<std::string> saveWrites(void);

    int titleCount(void) override;
    bool titleAt(int idx, HostTitle& out) override;
    int titleIndexOf(uint64_t id) override;
    std::string titleBackupPath(int idx, int kind) override;

    int savOpen(int titleIdx, int kind) override;
    int savOpenShared(uint64_t) override { return -1; }
    int savOpenExtdata(uint32_t) override { return -1; }
    int savOpenSystem(uint32_t) override { return -1; }
    bool savValid(int handle) override;
    int savRead(int handle, const char* path, char** outBuf, int* outSize) override;
    int savWrite(int handle, const char* path, const void* data, size_t size) override;
    int savDelete(int handle, const char* path) override;
    int savMkdir(int handle, const char* path) override;
    int savRmdir(int handle, const char* path) override;
    int savRename(int handle, const char* from, const char* to) override;
    bool savList(int handle, const std::string& dir, std::vector<HostDirEntry>& out) override;
    int savCommit(int handle) override;
    void savClose(int handle) override;
    void savCloseAll(void) override;

    int extdataDefaultId(uint64_t) override { return -1; }
    int extdataCreate(uint64_t, uint32_t, int, int) override { return -1; }
    int extdataDelete(uint32_t) override { return -1; }

    int deviceSecret(uint8_t* out, int want) override;
    bool randomBytes(void* out, size_t size) override;
    void lowerPriority(void) override {}

private:
    static constexpr int MAX_SAV_HANDLES = 8; // as on the console

    struct Slot {
        bool live = false;
        std::string dir;
    };

    std::string backupPath(const FakeTitle& title) const;
    std::string pathIn(int handle, const std::string& path) const;
    void record(std::string what);

    std::vector<FakeTitle> mTitles;
    Slot mSlots[MAX_SAV_HANDLES];
    std::mutex mMutex;
    std::vector<std::string> mWrites;
};

#endif
