// settings_backup.hpp - keeps the DECtalk settings of whoever runs the tests.
//
// test_settings, test_config_persist and test_pitch write and reset the real
// HKCU\Software\DECtalkDTC01, the key the configuration utility saves to;
// there is no registry sandbox for a command-line tool. Run on a machine that
// also speaks with DECtalk, they used to erase the settings made there.
//
// A SettingsBackup copies the key aside before a test touches it and, when it
// goes out of scope, makes the key exactly what was copied again -- or removes
// it, if there was no key to copy. A test stopped by a failed assert() puts the
// copy back from the abort signal; if the process dies some other way, the
// copy stays under HKCU\Software\DECtalkDTC01 test backup and the next
// SettingsBackup puts it back before taking its own.
#pragma once

#include <windows.h>

#include <csignal>

#include "user_settings.hpp"

namespace dectalk::test {

class SettingsBackup {
public:
    SettingsBackup() {
        restore();
        ok_ = take();
        std::signal(SIGABRT, [](int) { restore(); });
    }
    ~SettingsBackup() { restore(); }

    SettingsBackup(const SettingsBackup&) = delete;
    SettingsBackup& operator=(const SettingsBackup&) = delete;

    // False when the copy could not be made; the test must then leave the
    // settings alone.
    bool ok() const { return ok_; }

private:
    static constexpr const wchar_t* kBackupKey = L"Software\\DECtalkDTC01 test backup";
    static constexpr const wchar_t* kCopyKey = L"Settings";
    static constexpr const wchar_t* kComplete = L"Complete";

    static bool take() {
        HKEY backup = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kBackupKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &backup,
                            nullptr) != ERROR_SUCCESS) {
            return false;
        }
        bool copied = true;
        HKEY live = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, settings::ROOT_KEY, 0, KEY_READ, &live) == ERROR_SUCCESS) {
            HKEY copy = nullptr;
            copied = RegCreateKeyExW(backup, kCopyKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &copy, nullptr) ==
                         ERROR_SUCCESS &&
                     RegCopyTreeW(live, nullptr, copy) == ERROR_SUCCESS;
            if (copy) RegCloseKey(copy);
            RegCloseKey(live);
        }
        const DWORD one = 1;
        copied = copied && RegSetValueExW(backup, kComplete, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one),
                                          sizeof(one)) == ERROR_SUCCESS;
        RegCloseKey(backup);
        if (!copied) RegDeleteTreeW(HKEY_CURRENT_USER, kBackupKey);
        return copied;
    }

    // Puts a complete copy back. A copy without its Complete mark was cut
    // short while being taken, before the test touched anything, so it is
    // discarded and the settings key left as it is. A copy that could not be
    // put back is kept for the next run.
    static void restore() {
        HKEY backup = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kBackupKey, 0, KEY_READ, &backup) != ERROR_SUCCESS) return;
        DWORD complete = 0;
        DWORD size = sizeof(complete);
        bool done = true;
        if (RegQueryValueExW(backup, kComplete, nullptr, nullptr, reinterpret_cast<BYTE*>(&complete), &size) ==
                ERROR_SUCCESS &&
            complete == 1) {
            RegDeleteTreeW(HKEY_CURRENT_USER, settings::ROOT_KEY);
            HKEY copy = nullptr;
            if (RegOpenKeyExW(backup, kCopyKey, 0, KEY_READ, &copy) == ERROR_SUCCESS) {
                HKEY live = nullptr;
                done = RegCreateKeyExW(HKEY_CURRENT_USER, settings::ROOT_KEY, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                                       &live, nullptr) == ERROR_SUCCESS &&
                       RegCopyTreeW(copy, nullptr, live) == ERROR_SUCCESS;
                if (live) RegCloseKey(live);
                RegCloseKey(copy);
            }
        }
        RegCloseKey(backup);
        if (done) RegDeleteTreeW(HKEY_CURRENT_USER, kBackupKey);
    }

    bool ok_ = false;
};

}  // namespace dectalk::test
