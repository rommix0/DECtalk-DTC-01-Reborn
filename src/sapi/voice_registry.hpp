// voice_registry.hpp - SAPI5 voice-token registration for the 18 static
// DECtalk DTC-01 voices (dtc01::VOICES). Adapted from
// BstSpeech-sapi-master's src/voice_registry.hpp, trimmed to this driver's
// scope: no custom-voice tokens (out of scope for v1) and no
// install_selection dependency (all 18 voices are always published).
//
// Every voice is written as its own static token rather than produced by a
// dynamic token enumerator, which is what every SAPI5 client reads,
// including Windows Narrator.
#pragma once

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "registry.hpp"
#include "voices.hpp"

namespace dectalk {
namespace sapi {

inline constexpr const wchar_t* voices_path =
    L"Software\\Microsoft\\Speech\\Voices\\Tokens";

namespace detail {

[[nodiscard]] inline std::wstring ascii_to_wstring(const char* s)
{
    std::wstring w;
    for (const char* p = s; *p; ++p) {
        w += static_cast<wchar_t>(*p);
    }
    return w;
}

// "v20" -> "v2.0", "v18" -> "v1.8"; anything else (there is nothing else in
// dtc01::VOICES today) passes through unchanged rather than throwing, so a
// future firmware id doesn't hard-fail registration.
[[nodiscard]] inline std::wstring firmware_label(const char* firmware)
{
    if (std::strcmp(firmware, "v20") == 0) {
        return L"v2.0";
    }
    if (std::strcmp(firmware, "v18") == 0) {
        return L"v1.8";
    }
    return ascii_to_wstring(firmware);
}

}  // namespace detail

// Registry key name for this voice. Stable across releases (built from the
// voice's own key/firmware, not its display name), so a user's chosen voice
// survives an upgrade.
[[nodiscard]] inline std::wstring token_id_for(const dtc01::VoiceDef& v)
{
    std::wstring id = L"DECtalk_DTC01_";
    id += detail::ascii_to_wstring(v.firmware);
    id += L'_';
    id += detail::ascii_to_wstring(v.key);
    return id;
}

[[nodiscard]] inline std::wstring display_name_for(const dtc01::VoiceDef& v)
{
    std::wstring name = L"DECtalk DTC-01 - ";
    name += v.display;
    name += L" (";
    name += detail::firmware_label(v.firmware);
    name += L")";
    return name;
}

namespace detail {

// Writes one voice's token + Attributes subkey under `tokens`. Shared by both
// write_voice_tokens overloads so the actual registry shape lives in exactly
// one place.
inline void write_one_voice_token(dectalk::registry::key& tokens, const dtc01::VoiceDef& v,
                                   const std::wstring& clsid_str)
{
    using namespace dectalk::registry;

    const std::wstring name = display_name_for(v);

    key token(tokens, token_id_for(v), KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);
    token.set(name);
    token.set(L"CLSID", clsid_str);
    // SAPI looks a display name up under a value named for the LCID it is
    // asking about, falling back to the key's default value, set just above.
    token.set(L"409", name);

    key attrs(token, L"Attributes", KEY_SET_VALUE, true);
    attrs.set(L"Name", name);
    attrs.set(L"Gender", v.gender);
    attrs.set(L"Age", L"Adult");
    attrs.set(L"Language", L"409");
    attrs.set(L"Vendor", L"DECtalk");
    // Read back by SetObjectToken (Task B2), so the exact voice/firmware
    // pair is recovered without parsing a display name back apart.
    attrs.set(L"DtcVoice", detail::ascii_to_wstring(v.key));
    attrs.set(L"DtcFirmware", detail::ascii_to_wstring(v.firmware));
}

// True if `firmwares` is empty (no filter -> everything passes) or contains
// v.firmware.
[[nodiscard]] inline bool firmware_selected(const dtc01::VoiceDef& v,
                                             const std::vector<std::string>& firmwares)
{
    if (firmwares.empty()) {
        return true;
    }
    return std::find(firmwares.begin(), firmwares.end(), std::string(v.firmware)) !=
           firmwares.end();
}

}  // namespace detail

// Register only voices whose firmware id (v.firmware, e.g. "v20"/"v18") appears in
// `firmwares`. An EMPTY list means "no firmware filter" -> register all 18 (so a
// manual regsvr32 from a tree without the library beside the DLL still works).
inline void write_voice_tokens(HKEY root, const std::wstring& clsid_str,
                                const std::vector<std::string>& firmwares)
{
    using namespace dectalk::registry;

    key tokens(root, voices_path, KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);

    for (int i = 0; i < dtc01::voice_count(); ++i) {
        const dtc01::VoiceDef& v = dtc01::VOICES[i];
        if (!detail::firmware_selected(v, firmwares)) {
            continue;
        }
        detail::write_one_voice_token(tokens, v, clsid_str);
    }
}

// Every voice under `root` as its own static token. `root` is HKEY_LOCAL_MACHINE
// (or HKEY_CURRENT_USER, for a per-user install) in production; tests pass a
// throwaway key so nothing here ever has to touch the real Speech tree to be
// exercised.
inline void write_voice_tokens(HKEY root, const std::wstring& clsid_str)
{
    write_voice_tokens(root, clsid_str, std::vector<std::string>());
}

inline void remove_voice_tokens(HKEY root) noexcept
{
    using namespace dectalk::registry;

    try {
        key tokens(root, voices_path, KEY_ALL_ACCESS);
        for (int i = 0; i < dtc01::voice_count(); ++i) {
            const std::wstring id = token_id_for(dtc01::VOICES[i]);
            try {
                key token(tokens, id, KEY_ALL_ACCESS);
                token.delete_subkey(L"Attributes");
            }
            catch (...) {
            }
            try {
                tokens.delete_subkey(id);
            }
            catch (...) {
            }
        }
    }
    catch (...) {
    }
}

}
}
