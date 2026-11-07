#pragma once

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <share.h>

// Diagnostic log shared by the SAPI engine DLL, the DectalkConfig utility, and
// the native-core layer.
//
// Written to %LOCALAPPDATA%\DECtalkDTC01\dectalk-sapi.log, which is writable
// without elevation -- the install directory is not, and a SAPI engine runs
// inside whatever application is speaking. Every line carries the process
// name, its bitness and its pid, because this log is shared across process
// boundaries in a different way than a single-process log would be: multiple
// independent SAPI host processes (e.g. a screen reader and a media player
// both speaking at once), and the 32-bit and 64-bit builds of this engine
// running in different hosts, can all append to the same per-user file
// concurrently.
//
// The log records the text every Speak call sends to the firmware -- whatever
// a screen reader reads aloud -- so it stays off unless this registry value
// turns it on (the configuration utility's "Diagnostic log for bug reports"
// box writes it):
//   HKCU\Software\DECtalkDTC01  DWORD  Logging = 1
// Logging = 0, or no value at all, keeps it off. The value is cached, so the
// per-line hot path never touches the registry, and the engine re-reads it at
// the start of every utterance (RefreshEnabled() below): a change takes effect
// on the next thing spoken, without restarting the host.
//
// The file is capped and rotated to one previous copy, so leaving it on
// cannot fill a disk during a long session.

namespace DebugLog {

inline constexpr long MAX_LOG_BYTES = 4 * 1024 * 1024;

inline bool BuildLogPath(wchar_t* path, size_t size, const wchar_t* suffix)
{
    wchar_t base[MAX_PATH];
    // A 0 return is "not set"; a return >= MAX_PATH means the buffer wasn't
    // filled (the real value is longer) -- both fall through to %TEMP%.
    const DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) {
        if (GetTempPathW(MAX_PATH, base) == 0) {
            return false;
        }
    }
    wchar_t dir[MAX_PATH];
    if (swprintf_s(dir, L"%s\\DECtalkDTC01", base) < 0) {
        return false;
    }
    CreateDirectoryW(dir, nullptr);
    return swprintf_s(path, size, L"%s\\dectalk-sapi%s.log", dir, suffix) >= 0;
}

namespace detail {

// Atomic: the engine refreshes it at the start of each utterance, and a host
// can have several voices speaking, and so logging, on different threads.
inline std::atomic<int>& CachedEnabled()
{
    static std::atomic<int> cached{-1};
    return cached;
}

// Absent value (or absent key) means logging is off.
inline int ReadRegistryEnabled()
{
    int result = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\DECtalkDTC01", 0, KEY_READ, &key)
        == ERROR_SUCCESS) {
        DWORD value = 0;
        DWORD size = sizeof(value);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"Logging", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            result = (value != 0) ? 1 : 0;
        }
        RegCloseKey(key);
    }
    return result;
}

}  // namespace detail

// Read on first use and then cached: a hot path should not touch the registry
// per line.
inline bool Enabled()
{
    std::atomic<int>& cached = detail::CachedEnabled();
    int value = cached.load(std::memory_order_relaxed);
    if (value < 0) {
        value = detail::ReadRegistryEnabled();
        cached.store(value, std::memory_order_relaxed);
    }
    return value == 1;
}

// Re-reads HKCU\Software\DECtalkDTC01\Logging into the cached flag.
// DectalkTtsEngine::Speak calls it once per utterance, so turning the log on
// or off takes effect on the next thing spoken; tests call it to flip Logging
// within one process. Nothing on the per-line DECTALK_LOG path calls it.
inline void RefreshEnabled()
{
    detail::CachedEnabled().store(detail::ReadRegistryEnabled(), std::memory_order_relaxed);
}

inline const char* ProcessTag()
{
    static char tag[64] = {};
    if (!tag[0]) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* name = wcsrchr(exe, L'\\');
        name = name ? name + 1 : exe;
        char narrow[48] = {};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        // _snprintf_s with _TRUNCATE (not sprintf_s): sprintf_s on overflow
        // invokes the invalid-parameter handler and aborts the process --
        // fatal for a SAPI host whose exe basename happens to be long.
        // Truncating the tag is harmless; crashing the host on the first log
        // line is not.
        _snprintf_s(tag, sizeof(tag), _TRUNCATE, "%s/%d-bit pid %lu", narrow,
                    static_cast<int>(sizeof(void*) * 8), GetCurrentProcessId());
    }
    return tag;
}

inline void Rotate(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) {
        return;
    }
    if (info.nFileSizeHigh == 0 && info.nFileSizeLow < static_cast<DWORD>(MAX_LOG_BYTES)) {
        return;
    }
    wchar_t previous[MAX_PATH];
    if (BuildLogPath(previous, MAX_PATH, L".1")) {
        DeleteFileW(previous);
        MoveFileW(path, previous);
    }
}

// Speech latency is the whole point of this engine, so a log line costs one
// write and one flush rather than an open, a write and a close. The handle is
// kept for the life of the process; the flush is what keeps the file useful
// if the host crashes.
//
// The share mode matters more than it looks: _wfopen_s opens a file for
// EXCLUSIVE access, so the first process to log would lock every other one
// out for as long as it ran -- and since multiple SAPI host processes (and
// the 32-bit and 64-bit engines running inside different hosts) are all
// potentially long-lived, whichever started first would silently steal the
// log from everyone else. _wfsopen with _SH_DENYNO lets them all append.
// The open is also retried rather than latched, so a process that starts
// while the file is briefly unavailable still ends up logging.
inline FILE* Handle()
{
    static FILE* file = nullptr;
    static DWORD next_try = 0;

    if (!file) {
        const DWORD now = GetTickCount();
        if (next_try != 0 && now < next_try) {
            return nullptr;
        }
        next_try = now + 5000;

        wchar_t path[MAX_PATH];
        if (BuildLogPath(path, MAX_PATH, L"")) {
            Rotate(path);
            file = _wfsopen(path, L"a", _SH_DENYNO);
        }
    }
    return file;
}

inline void Log(const char* format, ...)
{
    if (!Enabled()) {
        return;
    }
    FILE* file = Handle();
    if (!file) {
        return;
    }

    // The whole line is composed first and written once: multiple SAPI host
    // processes (and the 32-bit and 64-bit engines running in different
    // hosts) can all append to this file at once, and a single write per
    // line keeps their output from interleaving mid-line.
    char line[2048];
    SYSTEMTIME now;
    GetLocalTime(&now);
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] ",
                        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                        now.wSecond, now.wMilliseconds, ProcessTag());
    if (n < 0) {
        return;
    }

    va_list args;
    va_start(args, format);
    const int m = _vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, format, args);
    va_end(args);
    if (m > 0) {
        n += m;
    }
    if (n < static_cast<int>(sizeof(line)) - 1) {
        line[n++] = '\n';
    }

    fwrite(line, 1, static_cast<size_t>(n), file);
    fflush(file);
}

}  // namespace DebugLog

#define DECTALK_LOG(...) DebugLog::Log(__VA_ARGS__)
