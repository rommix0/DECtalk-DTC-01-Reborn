#pragma once

#include <windows.h>
#include <algorithm>
#include <string>

#include "voices.hpp"

// User-adjustable DTC-01 speech settings, shared between the SAPI engine
// (Task C2, which reads these on every Speak) and the DTC-01 configuration
// utility (Task C4, which writes them).
//
// Everything lives under HKCU\Software\DECtalkDTC01. HKCU\Software is not
// WOW64-redirected, so a 32-bit utility and a 64-bit engine see the same
// values without any view games, and no elevation is ever required.
//
// A value that is absent means "use the built-in default," so a machine
// where the configuration utility has never been run behaves exactly like
// one built before these settings existed.
namespace dectalk {
namespace settings {

inline constexpr const wchar_t* ROOT_KEY   = L"Software\\DECtalkDTC01";
inline constexpr const wchar_t* VOICES_KEY = L"Software\\DECtalkDTC01\\Voices";

// ---------------------------------------------------------------------------
// Global settings. Rate and volume come from the SAPI client unless AppControl
// is off, when RatePercent and VolumeDB decide them instead. RateBoost is a
// percent of extra time-compression on top of either, and always applies.
// ---------------------------------------------------------------------------

inline constexpr int RATE_PERCENT_MIN = 25;
inline constexpr int RATE_PERCENT_MAX = 400;
inline constexpr int RATE_PERCENT_DEF = 100;
inline constexpr int VOLUME_DB_MIN    = -40;
inline constexpr int VOLUME_DB_MAX    = 12;
inline constexpr int VOLUME_DB_DEF    = 0;
inline constexpr int RATE_BOOST_MIN   = 0;
inline constexpr int RATE_BOOST_MAX   = 200;
inline constexpr int RATE_BOOST_DEF   = 0;

// "Allow SAPI5 apps to control rate, pitch and volume", on unless set to 0:
// the rate, volume and pitch the host asks for -- through ISpVoice and markup
// such as <pitch absmiddle> -- are what the engine uses. Off, the host's
// requests are ignored, and RatePercent, VolumeDB and each voice's Pitch
// slider decide instead.
inline constexpr const wchar_t* APP_CONTROL = L"AppControl";

struct GlobalSettings {
    int rate_percent = RATE_PERCENT_DEF;
    int volume_db = VOLUME_DB_DEF;
    int rate_boost = RATE_BOOST_DEF;
    bool app_control = true;
    std::string default_firmware = "v20";
};

// ---------------------------------------------------------------------------
// Per-voice overrides: the character voices' sliders, keyed the same way as
// the voice tokens (<firmware>_<key>, e.g. "v20_paul") so a user's tweaks
// survive an upgrade. Absent values keep the built-in character (50, the
// midpoint of every 0..100 slider).
// ---------------------------------------------------------------------------

inline constexpr int SLIDER_MIN = 0;
inline constexpr int SLIDER_MAX = 100;
inline constexpr int SLIDER_DEF = 50;

struct VoiceSettings {
    int inflection = SLIDER_DEF;
    int head_size = SLIDER_DEF;
    int breathiness = SLIDER_DEF;
    int richness = SLIDER_DEF;
    int smoothness = SLIDER_DEF;
    int loudness = SLIDER_DEF;
    int laryngealization = SLIDER_DEF;
    int assertiveness = SLIDER_DEF;
    int pitch = SLIDER_DEF;
};

namespace detail {

// REG_DWORD read as a signed int, clamped; absent or mistyped values fall
// back to the built-in default.
[[nodiscard]] inline int get_int(HKEY key, const wchar_t* name, int def, int lo, int hi) {
    DWORD type = 0;
    DWORD raw = 0;
    DWORD size = sizeof(raw);
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&raw), &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        return std::clamp(static_cast<int>(raw), lo, hi);
    }
    return def;
}

// REG_SZ read as a narrow string (values here are always ASCII, e.g. "v20").
[[nodiscard]] inline std::string get_string(HKEY key, const wchar_t* name, const std::string& def) {
    wchar_t buf[64] = {};
    DWORD type = 0;
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        std::string out;
        for (int i = 0; buf[i] != L'\0' && i < 63; ++i) {
            out += static_cast<char>(buf[i]);
        }
        return out;
    }
    return def;
}

[[nodiscard]] inline std::wstring widen(const char* s) {
    std::wstring out;
    for (const char* p = s; *p; ++p) {
        out += static_cast<wchar_t>(*p);
    }
    return out;
}

// "v20_paul": the name of a voice's subkey directly under VOICES_KEY.
[[nodiscard]] inline std::wstring voice_subkey_name(const char* voice_key, const char* firmware) {
    return widen(firmware) + L"_" + widen(voice_key);
}

[[nodiscard]] inline std::wstring voice_subkey_path(const char* voice_key, const char* firmware) {
    return std::wstring(VOICES_KEY) + L"\\" + voice_subkey_name(voice_key, firmware);
}

}  // namespace detail

[[nodiscard]] inline GlobalSettings load_global() {
    GlobalSettings s;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ROOT_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return s;
    }
    s.rate_percent = detail::get_int(key, L"RatePercent", RATE_PERCENT_DEF, RATE_PERCENT_MIN, RATE_PERCENT_MAX);
    s.volume_db = detail::get_int(key, L"VolumeDB", VOLUME_DB_DEF, VOLUME_DB_MIN, VOLUME_DB_MAX);
    s.rate_boost = detail::get_int(key, L"RateBoost", RATE_BOOST_DEF, RATE_BOOST_MIN, RATE_BOOST_MAX);
    s.app_control = detail::get_int(key, APP_CONTROL, 1, 0, 1) != 0;
    const std::string fw = detail::get_string(key, L"DefaultFirmware", "v20");
    s.default_firmware = (fw == "v20" || fw == "v18") ? fw : "v20";
    RegCloseKey(key);
    return s;
}

// One registry value name per slider, so absent values keep their defaults
// and a future slider costs nothing to add.
struct slider_field { const wchar_t* name; int VoiceSettings::*member; };

inline constexpr slider_field SLIDER_FIELDS[] = {
    { L"Inflection",       &VoiceSettings::inflection },
    { L"HeadSize",         &VoiceSettings::head_size },
    { L"Breathiness",      &VoiceSettings::breathiness },
    { L"Richness",         &VoiceSettings::richness },
    { L"Smoothness",       &VoiceSettings::smoothness },
    { L"Loudness",         &VoiceSettings::loudness },
    { L"Laryngealization", &VoiceSettings::laryngealization },
    { L"Assertiveness",    &VoiceSettings::assertiveness },
    { L"Pitch",            &VoiceSettings::pitch },
};

[[nodiscard]] inline VoiceSettings load_voice(const char* voice_key, const char* firmware) {
    VoiceSettings s;
    const std::wstring path = detail::voice_subkey_path(voice_key, firmware);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return s;
    }
    for (const slider_field& f : SLIDER_FIELDS) {
        s.*(f.member) = detail::get_int(key, f.name, SLIDER_DEF, SLIDER_MIN, SLIDER_MAX);
    }
    RegCloseKey(key);
    return s;
}

// ---------------------------------------------------------------------------
// Write side, used by the configuration utility. HKCU needs no elevation.
// ---------------------------------------------------------------------------

inline bool write_int(const std::wstring& subkey, const wchar_t* name, int value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD raw = static_cast<DWORD>(value);
    const bool ok = RegSetValueExW(key, name, 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&raw),
                                   sizeof(raw)) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

inline bool write_global_int(const wchar_t* name, int value) {
    return write_int(ROOT_KEY, name, value);
}

// Used for DefaultFirmware (REG_SZ, "v20" or "v18").
inline bool write_global_string(const wchar_t* name, const wchar_t* value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ROOT_KEY, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const size_t len = wcslen(value);
    const bool ok = RegSetValueExW(key, name, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(value),
                                   static_cast<DWORD>((len + 1) * sizeof(wchar_t)))
                    == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

inline bool write_voice_int(const char* voice_key, const char* firmware,
                            const wchar_t* name, int value) {
    return write_int(detail::voice_subkey_path(voice_key, firmware), name, value);
}

// Deletes one voice's overrides. Fails harmlessly if the voice was never
// customized (no subkey to delete).
inline void reset_voice(const char* voice_key, const char* firmware) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, VOICES_KEY, 0,
                      KEY_SET_VALUE | DELETE, &key) == ERROR_SUCCESS) {
        RegDeleteKeyW(key, detail::voice_subkey_name(voice_key, firmware).c_str());
        RegCloseKey(key);
    }
}

// Writes all of one voice's sliders, so it ends up matching `s` exactly.
inline bool write_voice(const char* voice_key, const char* firmware, const VoiceSettings& s) {
    bool ok = true;
    for (const slider_field& f : SLIDER_FIELDS) {
        ok = write_voice_int(voice_key, firmware, f.name, s.*(f.member)) && ok;
    }
    return ok;
}

// The configuration utility's "Apply settings to all voices" tick box. Only
// the utility reads it: while it is ticked the utility writes each change to
// every voice's own subkey, so the engine needs no notion of it.
inline constexpr const wchar_t* APPLY_TO_ALL_VOICES = L"ApplyToAllVoices";

[[nodiscard]] inline bool load_apply_to_all_voices() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ROOT_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    const bool on = detail::get_int(key, APPLY_TO_ALL_VOICES, 0, 0, 1) != 0;
    RegCloseKey(key);
    return on;
}

// The diagnostic log (debug_log.h), which saves the text of everything the
// engine speaks: off unless this is a nonzero REG_DWORD. The configuration
// utility's "Diagnostic log for bug reports" box reads and writes it here;
// debug_log.h reads it with the same rule itself, so it stays a standalone
// header, and the engine re-reads it at the start of every utterance.
inline constexpr const wchar_t* LOGGING = L"Logging";

[[nodiscard]] inline bool load_logging() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ROOT_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD raw = 0;
    DWORD size = sizeof(raw);
    const bool on = RegQueryValueExW(key, LOGGING, nullptr, &type, reinterpret_cast<LPBYTE>(&raw),
                                     &size) == ERROR_SUCCESS &&
                    type == REG_DWORD && raw != 0;
    RegCloseKey(key);
    return on;
}

// Removes every value and subkey the utility manages, and only those. That
// includes Logging, so Reset all settings also turns the diagnostic log off.
inline void reset_all() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ROOT_KEY, 0,
                      KEY_SET_VALUE | DELETE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"RatePercent");
        RegDeleteValueW(key, L"VolumeDB");
        RegDeleteValueW(key, L"RateBoost");
        RegDeleteValueW(key, L"DefaultFirmware");
        RegDeleteValueW(key, APPLY_TO_ALL_VOICES);
        RegDeleteValueW(key, APP_CONTROL);
        RegDeleteValueW(key, LOGGING);
        RegCloseKey(key);
    }
    for (const dtc01::VoiceDef& v : dtc01::VOICES) {
        reset_voice(v.key, v.firmware);
    }
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ROOT_KEY, 0,
                      KEY_SET_VALUE | DELETE, &root) == ERROR_SUCCESS) {
        RegDeleteKeyW(root, L"Voices");  // fails harmlessly if subkeys remain
        RegCloseKey(root);
    }
}

}  // namespace settings
}  // namespace dectalk
