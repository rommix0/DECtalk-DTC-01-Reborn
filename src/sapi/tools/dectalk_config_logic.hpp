// dectalk_config_logic.hpp - the control-value -> HKCU write logic for the
// DECtalk DTC-01 configuration utility, factored out of dectalk_config.cpp
// so it can be exercised with no window, dialog, or message loop at all
// (see tools/test_config_persist.cpp). Every function here only writes
// registry values via dectalk::settings -- dialog_proc in dectalk_config.cpp
// only decides *when* to call them (on WM_HSCROLL, CBN_SELCHANGE, ...), and
// pure GUI concerns (updating a readout, disabling a control) never happen in
// here.
#pragma once

#include "../user_settings.hpp"
#include "dectalk_config_res.h"

namespace dectalk_config {

// One entry per per-voice slider control: its dialog control id and the
// registry value name dectalk::settings::SLIDER_FIELDS (user_settings.hpp)
// expects for it. Keeping the mapping in a table, rather than a switch,
// means a slider's control id and its registry name only need to agree in
// one place.
struct voice_field {
    int id;
    const wchar_t* reg_name;
};

inline constexpr voice_field VOICE_FIELDS[] = {
    { IDC_PITCH,            L"Pitch" },
    { IDC_INFLECTION,       L"Inflection" },
    { IDC_HEADSIZE,         L"HeadSize" },
    { IDC_BREATHINESS,      L"Breathiness" },
    { IDC_RICHNESS,         L"Richness" },
    { IDC_SMOOTHNESS,       L"Smoothness" },
    { IDC_LOUDNESS,         L"Loudness" },
    { IDC_LARYNGEALIZATION, L"Laryngealization" },
    { IDC_ASSERTIVENESS,    L"Assertiveness" },
};

// Writes one per-voice slider's new value to HKCU for (voice_key, firmware).
// Returns false without writing anything if control_id isn't one of
// VOICE_FIELDS (e.g. a global control's id passed here by mistake).
[[nodiscard]] inline bool apply_setting(const char* voice_key, const char* firmware,
                                        int control_id, int value) {
    for (const voice_field& f : VOICE_FIELDS) {
        if (f.id == control_id) {
            return dectalk::settings::write_voice_int(voice_key, firmware, f.reg_name, value);
        }
    }
    return false;
}

// Writes one global int-valued control's new value to HKCU.
[[nodiscard]] inline bool apply_global_setting(int control_id, int value) {
    switch (control_id) {
    case IDC_RATE:      return dectalk::settings::write_global_int(L"RatePercent", value);
    case IDC_VOLUME:    return dectalk::settings::write_global_int(L"VolumeDB", value);
    case IDC_RATEBOOST: return dectalk::settings::write_global_int(L"RateBoost", value);
    default:            return false;
    }
}

// The one global control that isn't an int: the default-firmware combo.
// value is "v20" or "v18", matching dtc01::VoiceDef::firmware.
[[nodiscard]] inline bool apply_global_firmware(const wchar_t* firmware) {
    return dectalk::settings::write_global_string(L"DefaultFirmware", firmware);
}

// ---------------------------------------------------------------------------
// "Apply settings to all voices". Voice sliders are relative to each voice's
// own factory default (50 is that voice's own value), so the same slider
// positions on every voice adjust them all the same way -- which is what
// someone who has tuned one voice wants the rest to get. "All voices" is
// every entry in dtc01::VOICES, both firmware versions.
// ---------------------------------------------------------------------------

// apply_setting() for every voice. Returns false, writing nothing, if
// control_id isn't a voice slider.
[[nodiscard]] inline bool apply_setting_to_all_voices(int control_id, int value) {
    bool ok = true;
    for (const dtc01::VoiceDef& v : dtc01::VOICES) {
        ok = apply_setting(v.key, v.firmware, control_id, value) && ok;
    }
    return ok;
}

// Gives every voice the sliders (voice_key, firmware) has now.
[[nodiscard]] inline bool copy_voice_to_all_voices(const char* voice_key, const char* firmware) {
    const dectalk::settings::VoiceSettings source =
        dectalk::settings::load_voice(voice_key, firmware);
    bool ok = true;
    for (const dtc01::VoiceDef& v : dtc01::VOICES) {
        ok = dectalk::settings::write_voice(v.key, v.firmware, source) && ok;
    }
    return ok;
}

// Every voice back to its own factory defaults.
inline void reset_all_voices() {
    for (const dtc01::VoiceDef& v : dtc01::VOICES) {
        dectalk::settings::reset_voice(v.key, v.firmware);
    }
}

// Remembers the tick box, so the utility reopens the way it was left.
[[nodiscard]] inline bool set_apply_to_all_voices(bool on) {
    return dectalk::settings::write_global_int(dectalk::settings::APPLY_TO_ALL_VOICES, on ? 1 : 0);
}

// "Allow SAPI5 apps to control rate, pitch and volume". Unlike the tick box
// above, the engine reads this one (GlobalSettings::app_control) on every
// utterance.
[[nodiscard]] inline bool set_app_control(bool on) {
    return dectalk::settings::write_global_int(dectalk::settings::APP_CONTROL, on ? 1 : 0);
}

// "Diagnostic log for bug reports". The engine re-reads it at the start of
// every utterance, so the log starts or stops with the next thing spoken.
[[nodiscard]] inline bool set_logging(bool on) {
    return dectalk::settings::write_global_int(dectalk::settings::LOGGING, on ? 1 : 0);
}

}  // namespace dectalk_config
