#pragma once

#include <string>
#include <cstring>

namespace dtc01 {

struct VoiceDef {
    const char* key;         // unique lowercase key (e.g., "paul")
    const char* mnemonic;    // firmware command mnemonic (e.g., "np")
    const wchar_t* display;  // display name (e.g., L"Perfect Paul")
    const wchar_t* gender;   // L"Male" or L"Female"
    const char* firmware;    // "v20" or "v18"
};

// 18-voice table: 10 v20 entries + 8 v18 entries
inline constexpr VoiceDef VOICES[18] = {
    // v20 voices (10 total)
    {"paul", "np", L"Perfect Paul", L"Male", "v20"},
    {"betty", "nb", L"Beautiful Betty", L"Female", "v20"},
    {"harry", "nh", L"Huge Harry", L"Male", "v20"},
    {"frank", "nf", L"Frail Frank", L"Male", "v20"},
    {"dennis", "nd", L"Doctor Dennis", L"Male", "v20"},
    {"kit", "nk", L"Kit the Kid", L"Female", "v20"},
    {"rita", "nr", L"Rough Rita", L"Female", "v20"},
    {"ursula", "nu", L"Uppity Ursula", L"Female", "v20"},
    {"wendy", "nw", L"Whispery Wendy", L"Female", "v20"},
    {"val", "nv", L"Variable Val", L"Male", "v20"},

    // v18 voices (8 total - excludes dennis and wendy)
    {"paul", "np", L"Perfect Paul", L"Male", "v18"},
    {"betty", "nb", L"Beautiful Betty", L"Female", "v18"},
    {"harry", "nh", L"Huge Harry", L"Male", "v18"},
    {"frank", "nf", L"Frail Frank", L"Male", "v18"},
    {"kit", "nk", L"Kit the Kid", L"Female", "v18"},
    {"rita", "nr", L"Rough Rita", L"Female", "v18"},
    {"ursula", "nu", L"Uppity Ursula", L"Female", "v18"},
    {"val", "nv", L"Variable Val", L"Male", "v18"},
};

inline int voice_count() {
    return 18;
}

inline const VoiceDef* find_voice(const std::string& key, const std::string& firmware) {
    for (int i = 0; i < 18; ++i) {
        if (VOICES[i].key == key &&
            std::strcmp(VOICES[i].firmware, firmware.c_str()) == 0) {
            return &VOICES[i];
        }
    }
    return nullptr;
}

} // namespace dtc01
