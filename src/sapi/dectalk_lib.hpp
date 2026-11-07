// dectalk_lib.hpp - the speech library (DECtalk.dll, src/api) as the SAPI engine uses it (REFERENCE.md s18.21).
//
// The library runs one engine per loaded module, started once per process, and the firmware (v1.8 or v2.0) is
// chosen at its Startup. SAPI hosts switch voices freely, between firmwares too, so each firmware has its own copy
// of the library: DECtalk.dll for v1.8 and the same file as DECtalk20.dll for v2.0, each with its own globals. Each
// is started on first use, with the manual clock (TTS_MANUAL_CLOCK: the caller's thread runs the synthesizer, as
// SAPI's Speak pulls audio) into memory buffers, and kept until the process ends.
// ponytail: one copy per firmware; a second instance per module needs the firmware's globals in the handle (s17.10).
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "ttsapi.h"

namespace dtc01 {

class Library {
public:
    // The library for "v18" or "v20", started on first use; nullptr if its DLL cannot be loaded or started.
    // Hold mutex() around every call below.
    static Library* get(const std::string& firmware);

    std::mutex& mutex() { return mu_; }

    // Queue text (TTS_FORCE: the clause is ended, so it is spoken now).
    bool speak(const std::string& text);

    // Run the synthesizer for up to max samples into `out`; marks reached in them go to `marks` (each with the
    // index of its first sample in `out`). Returns the samples made; 0 once everything queued has been said.
    size_t run(std::vector<int16_t>& out, size_t max, std::vector<std::pair<uint32_t, size_t>>& marks);

    // Drop what is queued and what is not yet delivered (DT_STOP).
    void stop();

private:
    Library() = default;
    bool start(const std::wstring& dll, bool v20);
    void take(LPTTS_BUFFER_T b, std::vector<int16_t>& out, std::vector<std::pair<uint32_t, size_t>>& marks);

    using StartupEx_t = MMRESULT (*)(LPTTS_HANDLE_T*, UINT, DWORD, TTS_CALLBACK_T, LONG);
    using Speak_t = MMRESULT (*)(LPTTS_HANDLE_T, LPSTR, DWORD);
    using Reset_t = MMRESULT (*)(LPTTS_HANDLE_T, BOOL);
    using Status_t = MMRESULT (*)(LPTTS_HANDLE_T, LPDWORD, LPDWORD, DWORD);
    using Open_t = MMRESULT (*)(LPTTS_HANDLE_T, DWORD);
    using Buffer_t = MMRESULT (*)(LPTTS_HANDLE_T, LPTTS_BUFFER_T);
    using Return_t = MMRESULT (*)(LPTTS_HANDLE_T, LPTTS_BUFFER_T*);
    using Run_t = MMRESULT (*)(LPTTS_HANDLE_T, DWORD);

    std::mutex mu_;
    LPTTS_HANDLE_T h_ = nullptr;
    Speak_t speak_ = nullptr;
    Reset_t reset_ = nullptr;
    Status_t status_ = nullptr;
    Buffer_t add_ = nullptr;
    Return_t return_ = nullptr;
    Run_t run_ = nullptr;
    uint64_t delivered_ = 0;             // samples taken from buffers since Startup: the library's sample numbers
    TTS_BUFFER_T buf_{};
    std::vector<int16_t> data_;
    std::vector<TTS_INDEX_T> index_;
};

}  // namespace dtc01
