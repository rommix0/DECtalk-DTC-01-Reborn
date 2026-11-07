// text_pipeline.cpp - see text_pipeline.hpp. Every table and formula here is
// ported verbatim from addon/synthDrivers/dectalkDtc01/protocol/commands.py;
// keep the two in sync (a later task diffs their outputs byte-for-byte).
#include "text_pipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <regex>

namespace dtc01 {

namespace {

constexpr int RATE_MIN_WPM = 120;
constexpr int RATE_MAX_WPM = 350;

// Python's round() is round-half-to-even ("banker's rounding"), unlike
// C++'s std::round (round-half-away-from-zero). commands.py relies on
// round() throughout (scale_from_default, clamp), so a value landing exactly
// on .5 would come out differently under std::round. Every input here is
// non-negative, so this only needs to handle that case.
long long py_round(double x) {
    double floor_val = std::floor(x);
    double diff = x - floor_val;
    long long fl = static_cast<long long>(floor_val);
    if (diff < 0.5) return fl;
    if (diff > 0.5) return fl + 1;
    return (fl % 2 == 0) ? fl : fl + 1;  // exactly .5: round to even
}

// DV_PARAMS (commands.py), restricted to the nine abbreviations DvParams
// exposes. Keyed by abbreviation here (rather than commands.py's full
// descriptive name) since that's the only thing clamp() actually needs the
// name for: looking up this same (min, max) pair.
struct ClampRange { double lo, hi; };

const ClampRange& clamp_range(const std::string& abbr) {
    static const std::array<std::pair<const char*, ClampRange>, 9> kRanges{{
        {"ap", {30, 300}},
        {"as", {0, 100}},
        {"br", {0, 72}},
        {"hs", {40, 200}},
        {"la", {0, 100}},
        {"pr", {0, 250}},
        {"ri", {0, 100}},
        {"sm", {0, 100}},
        {"g5", {0, 80}},
    }};
    for (const auto& entry : kRanges) {
        if (abbr == entry.first) return entry.second;
    }
    return kRanges[0].second;  // unreachable for the callers below
}

int clamp(const std::string& abbr, double value) {
    const ClampRange& r = clamp_range(abbr);
    double bounded = std::min(std::max(value, r.lo), r.hi);
    return static_cast<int>(py_round(bounded));
}

// VOICE_PARAM_DEFAULTS (commands.py), copied verbatim: (default, min, max)
// per voice per DV abbreviation.
struct VoiceParamEntry { const char* abbr; int def, lo, hi; };
struct VoiceDefaults { const char* voice; std::array<VoiceParamEntry, 9> params; };

const std::array<VoiceDefaults, 10> kVoiceParamDefaults{{
    {"paul",   {{{"g5", 72, 0, 80}, {"ap", 120, 30, 300}, {"pr", 100, 0, 250}, {"hs", 100, 40, 200}, {"br", 0, 0, 72}, {"ri", 80, 0, 100}, {"sm", 54, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
    {"betty",  {{{"g5", 68, 0, 80}, {"ap", 180, 30, 300}, {"pr", 160, 0, 250}, {"hs", 100, 40, 200}, {"br", 46, 0, 72}, {"ri", 0, 0, 100}, {"sm", 44, 0, 100}, {"la", 0, 0, 100}, {"as", 65, 0, 100}}}},
    {"harry",  {{{"g5", 69, 0, 80}, {"ap", 78, 30, 300}, {"pr", 50, 0, 250}, {"hs", 120, 40, 200}, {"br", 0, 0, 72}, {"ri", 86, 0, 100}, {"sm", 34, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
    {"frank",  {{{"g5", 74, 0, 80}, {"ap", 153, 30, 300}, {"pr", 90, 0, 250}, {"hs", 90, 40, 200}, {"br", 50, 0, 72}, {"ri", 80, 0, 100}, {"sm", 36, 0, 100}, {"la", 12, 0, 100}, {"as", 65, 0, 100}}}},
    {"dennis", {{{"g5", 74, 0, 80}, {"ap", 100, 30, 300}, {"pr", 135, 0, 250}, {"hs", 105, 40, 200}, {"br", 62, 0, 72}, {"ri", 0, 0, 100}, {"sm", 100, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
    {"kit",    {{{"g5", 62, 0, 80}, {"ap", 306, 30, 300}, {"pr", 180, 0, 250}, {"hs", 80, 40, 200}, {"br", 40, 0, 72}, {"ri", 40, 0, 100}, {"sm", 44, 0, 100}, {"la", 0, 0, 100}, {"as", 65, 0, 100}}}},
    {"rita",   {{{"g5", 72, 0, 80}, {"ap", 106, 30, 300}, {"pr", 80, 0, 250}, {"hs", 95, 40, 200}, {"br", 49, 0, 72}, {"ri", 0, 0, 100}, {"sm", 34, 0, 100}, {"la", 4, 0, 100}, {"as", 65, 0, 100}}}},
    {"ursula", {{{"g5", 69, 0, 80}, {"ap", 264, 30, 300}, {"pr", 135, 0, 250}, {"hs", 95, 40, 200}, {"br", 0, 0, 72}, {"ri", 100, 0, 100}, {"sm", 64, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
    {"wendy",  {{{"g5", 80, 0, 80}, {"ap", 264, 30, 300}, {"pr", 135, 0, 250}, {"hs", 95, 40, 200}, {"br", 58, 0, 72}, {"ri", 0, 0, 100}, {"sm", 100, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
    {"val",    {{{"g5", 72, 0, 80}, {"ap", 120, 30, 300}, {"pr", 100, 0, 250}, {"hs", 100, 40, 200}, {"br", 0, 0, 72}, {"ri", 80, 0, 100}, {"sm", 54, 0, 100}, {"la", 0, 0, 100}, {"as", 100, 0, 100}}}},
}};

const VoiceDefaults& voice_defaults_for(const std::string& voice_key) {
    for (const auto& v : kVoiceParamDefaults) {
        if (voice_key == v.voice) return v;
    }
    for (const auto& v : kVoiceParamDefaults) {  // fall back to "paul"
        if (std::string("paul") == v.voice) return v;
    }
    return kVoiceParamDefaults[0];  // unreachable
}

// voice_param(voice, param) (commands.py): (default, min, max), with the
// range widened to include the default itself (Kit's average pitch default
// of 306 sits above the reported 30..300 maximum).
void voice_param(const std::string& voice_key, const std::string& abbr,
                  int* out_default, int* out_lo, int* out_hi) {
    const VoiceDefaults& v = voice_defaults_for(voice_key);
    for (const auto& p : v.params) {
        if (abbr == p.abbr) {
            *out_default = p.def;
            *out_lo = std::min(p.lo, p.def);
            *out_hi = std::max(p.hi, p.def);
            return;
        }
    }
    *out_default = 0;
    *out_lo = 0;
    *out_hi = 0;
}

// scale_from_default(voice, param, slider) (commands.py): piecewise-linear
// map of NVDA's 0..100 slider onto the parameter, 50 = this voice's default.
int scale_from_default(const std::string& voice_key, const std::string& abbr, int slider) {
    int default_v, lo, hi;
    voice_param(voice_key, abbr, &default_v, &lo, &hi);
    slider = std::max(0, std::min(100, slider));
    if (slider == 50) return default_v;
    if (slider < 50) {
        return static_cast<int>(py_round(lo + (default_v - lo) * (slider / 50.0)));
    }
    return static_cast<int>(py_round(default_v + (hi - default_v) * ((slider - 50) / 50.0)));
}

}  // namespace

std::string rate_command(int wpm) {
    int clamped = std::min(std::max(wpm, RATE_MIN_WPM), RATE_MAX_WPM);
    return "[:ra " + std::to_string(clamped) + "]";
}

std::string voice_command(const char* mnemonic) {
    return std::string("[:") + mnemonic + "]";
}

std::string dv_command(const std::string& voice_key, const DvParams& p) {
    // Mirrors __init__.py::_designVoiceCommand exactly: a slider left at 50
    // (this voice's own default) is skipped entirely -- `if sliderValue ==
    // 50: continue` -- and if nothing differs, no [:dv ...] is sent at all
    // -- `if not params: return ""`. Measured against the real firmware:
    // sending the full 9-token form (even with every value at the voice's
    // own default) breaks synthesis outright, so this is not merely a
    // shorter-prefix optimization.
    //
    // Order matches commands.py's design_voice_command() as driven by
    // __init__.py::_designVoiceCommand's own iteration order: ap, pr, hs,
    // br, ri, sm, g5, then laryngealization/assertiveness (la, as), which
    // that driver method doesn't currently expose but DV_PARAMS supports.
    struct Token { const char* abbr; int slider; };
    const Token tokens[] = {
        {"ap", p.pitch},
        {"pr", p.inflection},
        {"hs", p.head_size},
        {"br", p.breathiness},
        {"ri", p.richness},
        {"sm", p.smoothness},
        {"g5", p.loudness},
        {"la", p.laryngealization},
        {"as", p.assertiveness},
    };
    std::string out;
    for (const auto& t : tokens) {
        if (t.slider == 50) continue;  // this voice's own default: omit
        int scaled = scale_from_default(voice_key, t.abbr, t.slider);
        int value = clamp(t.abbr, scaled);
        out += " ";
        out += t.abbr;
        out += " ";
        out += std::to_string(value);
    }
    if (out.empty()) return "";
    return "[:dv" + out + "]";
}

namespace {

// dv_command's token order, which DvValues follows.
constexpr const char* kDvOrder[9] = {"ap", "pr", "hs", "br", "ri", "sm", "g5", "la", "as"};

}  // namespace

DvValues dv_defaults(const std::string& voice_key) {
    DvValues out{};
    for (int i = 0; i < 9; ++i) {
        int lo = 0, hi = 0;
        voice_param(voice_key, kDvOrder[i], &out.value[i], &lo, &hi);
    }
    return out;
}

DvValues dv_values(const std::string& voice_key, const DvParams& p) {
    const int sliders[9] = {p.pitch, p.inflection, p.head_size, p.breathiness, p.richness,
                            p.smoothness, p.loudness, p.laryngealization, p.assertiveness};
    DvValues out = dv_defaults(voice_key);
    for (int i = 0; i < 9; ++i) {
        if (sliders[i] == 50) continue;
        const int scaled = scale_from_default(voice_key, kDvOrder[i], sliders[i]);
        if (scaled != out.value[i]) {
            out.value[i] = clamp(kDvOrder[i], scaled);
        }
    }
    return out;
}

std::string dv_change_command(const DvValues& held, const DvValues& wanted, bool* needs_voice) {
    if (needs_voice) *needs_voice = false;
    std::string out;
    for (int i = 0; i < 9; ++i) {
        const int v = wanted.value[i];
        if (v == held.value[i]) continue;
        if (clamp(kDvOrder[i], v) != v) {
            if (needs_voice) *needs_voice = true;
            continue;
        }
        out += " ";
        out += kDvOrder[i];
        out += " ";
        out += std::to_string(v);
    }
    if (out.empty()) return "";
    return "[:dv" + out + "]";
}

std::string sanitize_text(const std::string& utf8) {
    std::string text = utf8;
    for (char& ch : text) {
        if (ch == '[' || ch == ']') ch = ' ';
    }
    // commands.py: _ISOLATED_OPEN_PAREN = re.compile(r"\((?=\s|$)")
    static const std::regex isolated_open(R"(\((?=\s|$))");
    text = std::regex_replace(text, isolated_open, " ");
    // commands.py: _ISOLATED_CLOSE_PAREN = re.compile(r"(^|\s)\)")
    static const std::regex isolated_close(R"((^|\s)\))");
    text = std::regex_replace(text, isolated_close, "$1 ");
    return text;
}

}  // namespace dtc01
