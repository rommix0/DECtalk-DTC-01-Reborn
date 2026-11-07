// dectalk_lib.cpp - see dectalk_lib.hpp.
#include "dectalk_lib.hpp"

#include <windows.h>

#include <algorithm>
#include <map>
#include <memory>

#include "debug_log.h"

namespace dtc01 {

namespace {

constexpr size_t kBufferSamples = 1000;   // 100 ms: the most one run() step makes
constexpr size_t kMaxMarks = 64;

void module_anchor() {}

// This module's directory (the SAPI DLL, or a tool that links the engine), with a trailing '\'.
std::wstring module_dir()
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&module_anchor), &self)) {
        return {};
    }
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    std::wstring path(buf, n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

// The started libraries outlive any engine object, so this module must stay loaded: unloaded and loaded again, it
// would find its library already started (MMSYSERR_ALLOCATED) without the handle.
void pin_module()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&module_anchor), &self);
}

template <class F>
bool resolve(HMODULE m, const char* name, F& fn)
{
    fn = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    return fn != nullptr;
}

}  // namespace

Library* Library::get(const std::string& firmware)
{
    static std::mutex table_mu;
    static std::map<std::string, std::unique_ptr<Library>> table;  // also the failed ones (nullptr): tried once
    std::lock_guard<std::mutex> lk(table_mu);
    const auto it = table.find(firmware);
    if (it != table.end()) {
        return it->second.get();
    }
    const bool v20 = firmware == "v20";
    std::unique_ptr<Library> lib(new Library());
    const std::wstring dll = module_dir() + (v20 ? L"DECtalk20.dll" : L"DECtalk.dll");
    if (!lib->start(dll, v20)) {
        DECTALK_LOG("Library: could not start %ls (firmware %s)", dll.c_str(), firmware.c_str());
        lib.reset();
    } else {
        pin_module();
    }
    return (table[firmware] = std::move(lib)).get();
}

bool Library::start(const std::wstring& dll, bool v20)
{
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) {
        return false;
    }
    StartupEx_t startup = nullptr;
    Open_t open = nullptr;
    if (!resolve(m, "TextToSpeechStartupEx", startup) || !resolve(m, "TextToSpeechSpeak", speak_) ||
        !resolve(m, "TextToSpeechReset", reset_) || !resolve(m, "TextToSpeechGetStatus", status_) ||
        !resolve(m, "TextToSpeechOpenInMemory", open) || !resolve(m, "TextToSpeechAddBuffer", add_) ||
        !resolve(m, "TextToSpeechReturnBuffer", return_) || !resolve(m, "TextToSpeechRun", run_)) {
        FreeLibrary(m);
        return false;
    }
    const DWORD options = DO_NOT_USE_AUDIO_DEVICE | TTS_MANUAL_CLOCK | (v20 ? TTS_FIRMWARE_V20 : 0);
    if (startup(&h_, WAVE_MAPPER, options, nullptr, 0) != MMSYSERR_NOERROR) {
        return false;                   // the module stays loaded: a started-and-failed engine cannot be unloaded
    }
    if (open(h_, WAVE_FORMAT_1M16) != MMSYSERR_NOERROR) {
        return false;
    }
    data_.resize(kBufferSamples);
    index_.resize(kMaxMarks);
    buf_.lpData = reinterpret_cast<LPSTR>(data_.data());
    buf_.lpIndexArray = index_.data();
    buf_.dwMaximumNumberOfIndexMarks = static_cast<DWORD>(index_.size());
    return true;
}

bool Library::speak(const std::string& text)
{
    std::string s = text;
    return speak_(h_, s.data(), TTS_FORCE) == MMSYSERR_NOERROR;
}

void Library::take(LPTTS_BUFFER_T b, std::vector<int16_t>& out, std::vector<std::pair<uint32_t, size_t>>& marks)
{
    const size_t n = b->dwBufferLength / sizeof(int16_t);
    const size_t at = out.size();
    const int16_t* p = reinterpret_cast<const int16_t*>(b->lpData);
    out.insert(out.end(), p, p + n);
    for (DWORD i = 0; i < b->dwNumberOfIndexMarks; ++i) {
        if (b->lpIndexArray[i].dwReserved != TTS_INDEX_MARK) {
            continue;
        }
        // Sample numbers count from Startup, as delivered_ does (every sample made goes into a buffer).
        const int64_t rel = static_cast<int64_t>(b->lpIndexArray[i].dwIndexSampleNumber) -
                            static_cast<int64_t>(delivered_);
        marks.emplace_back(b->lpIndexArray[i].dwIndexValue,
                           at + static_cast<size_t>(std::clamp<int64_t>(rel, 0, static_cast<int64_t>(n))));
    }
    delivered_ += n;
}

size_t Library::run(std::vector<int16_t>& out, size_t max, std::vector<std::pair<uint32_t, size_t>>& marks)
{
    out.clear();
    marks.clear();
    while (out.size() < max) {
        // Never filled: in 32-bit builds a full buffer goes to the callback (TTS_MSG_BUFFER carries its address,
        // as in dapi), and ReturnBuffer would have none. One sample short of full, it comes back from ReturnBuffer.
        const size_t want = std::min(max - out.size(), data_.size() - 1);
        buf_.dwMaximumBufferLength = static_cast<DWORD>(data_.size() * sizeof(int16_t));
        buf_.dwBufferLength = 0;
        buf_.dwNumberOfIndexMarks = 0;
        add_(h_, &buf_);
        run_(h_, static_cast<DWORD>(want));
        LPTTS_BUFFER_T got = nullptr;
        return_(h_, &got);              // the buffer back, as far as it was filled
        if (!got) {
            break;
        }
        const size_t before = out.size();
        take(got, out, marks);
        if (out.size() - before < want) {
            break;                      // it has said everything queued
        }
    }
    return out.size();
}

void Library::stop()
{
    reset_(h_, FALSE);
}

}  // namespace dtc01
