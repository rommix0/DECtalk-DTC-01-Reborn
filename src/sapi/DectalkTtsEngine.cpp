// DectalkTtsEngine.cpp - see DectalkTtsEngine.hpp for the design rationale.
#include "DectalkTtsEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "debug_log.h"
#include "dectalk_lib.hpp"
#include "ratebooster.hpp"
#include "text_pipeline.hpp"
#include "user_settings.hpp"
#include "utils.hpp"

namespace dtc01 {
namespace sapi {

namespace {

// Fixed native output format (design spec §4.2): mono, signed 16-bit PCM at
// the DTC-01 DAC rate. SAPI's audio layer resamples to the device as needed.
constexpr WORD  AUDIO_FORMAT_TAG   = WAVE_FORMAT_PCM;  // 1
constexpr WORD  AUDIO_CHANNELS     = 1;
constexpr DWORD AUDIO_SAMPLE_RATE  = 10000;
constexpr WORD  AUDIO_BITS         = 16;

// SAPI rate/pitch arrive on a -10..+10 scale.
constexpr int SAPI_RATE_MIN = -10;
constexpr int SAPI_RATE_MAX =  10;

// The DTC-01 word-per-minute range the firmware honours (text_pipeline clamps
// to the same window; keep in step with commands.py RATE_MIN/MAX_WPM).
constexpr int DEFAULT_WPM = 180;

// The pump runs the library 25 ms at a time: an abort, a rate or volume change is noticed within 25 ms of audio.
constexpr size_t kBlockSamples = 250;

// Longest single Write; the host is polled for an abort between writes.
constexpr size_t kWriteChunkSamples = 1000;

// Safety net against an engine that never settles (it always does: the library says what it is given and stops).
constexpr size_t kPieceCapSamples = 600 * AUDIO_SAMPLE_RATE;

// SAPI rate -10..+10 -> words per minute, 0 == DEFAULT_WPM. rate_command()
// clamps to the firmware's real window, so out-of-band values are safe here.
int sapi_rate_to_wpm(int sapi_rate)
{
    const int clamped = std::clamp(sapi_rate, SAPI_RATE_MIN, SAPI_RATE_MAX);
    const double wpm = DEFAULT_WPM * std::pow(2.0, clamped / 10.0);
    return static_cast<int>(std::lround(wpm));
}

void add_event(ISpTTSEngineSite* site, SPEVENTENUM id, ULONGLONG offset,
               WPARAM wparam, LPARAM lparam, SPEVENTLPARAMTYPE ltype)
{
    SPEVENT ev = {};
    ev.eEventId = id;
    ev.elParamType = ltype;
    ev.ullAudioStreamOffset = offset;
    ev.ulStreamNum = 0;
    ev.wParam = wparam;
    ev.lParam = lparam;
    site->AddEvents(&ev, 1);
}

// The 0..100 percent applied to the samples. While the host controls volume
// (the default) that is the host's volume, ISpVoice::SetVolume, scaled by the
// fragment's own from <volume> markup; with "Allow SAPI5 apps to control rate,
// pitch and volume" unticked it is the configuration utility's VolumeDB as a
// gain on full volume, and the host's requests are ignored.
struct Loudness {
    bool app_control = true;
    USHORT host = 100;   // re-read on SPVES_VOLUME
    ULONG fragment = 100;
    int volume_db = dectalk::settings::VOLUME_DB_DEF;

    [[nodiscard]] int percent() const
    {
        if (app_control) {
            return std::clamp<int>(static_cast<int>(host) * fragment / 100, 0, 100);
        }
        // 20*log10(percent/100) = volume_db; 0 dB is full volume.
        const double gained = 100.0 * std::pow(10.0, volume_db / 20.0);
        return std::clamp(static_cast<int>(std::lround(gained)), 0, 100);
    }
};

// Everything on its way from the pump to the host. Applies the volume, time-compresses it when rate boost is on,
// and writes in bounded chunks with an abort check before each.
class SiteWriter
{
public:
    SiteWriter(ISpTTSEngineSite* site, double rate_boost) : site_(site), booster_(rate_boost), boost_(rate_boost) {}

    // Where the sample `samples` from the next write will land in the stream (rate boost shortens the audio).
    [[nodiscard]] ULONGLONG offset_of(size_t samples) const
    {
        const double n = booster_.active() ? samples / boost_ : static_cast<double>(samples);
        return bytes_ + static_cast<ULONGLONG>(n) * sizeof(int16_t);
    }

    void set_volume(int percent) { volume_ = percent; }

    // Bytes written so far: the stream offset for events.
    [[nodiscard]] ULONGLONG bytes() const { return bytes_; }

    // False once the host has aborted or a Write failed.
    bool write(const int16_t* samples, size_t count)
    {
        scaled_.assign(samples, samples + count);
        if (volume_ < 100) {
            for (int16_t& s : scaled_) {
                s = static_cast<int16_t>((static_cast<int32_t>(s) * volume_) / 100);
            }
        }
        if (!booster_.active()) {
            return send(scaled_.data(), scaled_.size());
        }
        boosted_.clear();
        booster_.push(scaled_.data(), scaled_.size(), boosted_);
        return send(boosted_.data(), boosted_.size());
    }

    bool write_silence(size_t samples)
    {
        silence_.assign(std::min(samples, kWriteChunkSamples), 0);
        while (samples > 0) {
            const size_t n = std::min(samples, silence_.size());
            if (!send(silence_.data(), n)) {
                return false;
            }
            samples -= n;
        }
        return true;
    }

    // End of a fragment: releases what the rate booster is still holding.
    bool finish_fragment()
    {
        if (!booster_.active()) {
            return true;
        }
        boosted_.clear();
        booster_.finish(boosted_);
        return send(boosted_.data(), boosted_.size());
    }

private:
    bool send(const int16_t* pcm, size_t count)
    {
        for (size_t offset = 0; offset < count;) {
            if (site_->GetActions() & SPVES_ABORT) {
                return false;
            }
            const size_t chunk = std::min(count - offset, kWriteChunkSamples);
            ULONG written = 0;
            // A failed Write is treated exactly like SPVES_ABORT.
            if (FAILED(site_->Write(pcm + offset, static_cast<ULONG>(chunk * sizeof(int16_t)),
                                    &written))) {
                return false;
            }
            bytes_ += written;
            offset += chunk;
        }
        return true;
    }

    ISpTTSEngineSite* site_;
    dtc01::TimeCompressor booster_;
    double boost_;
    int volume_ = 100;
    ULONGLONG bytes_ = 0;
    std::vector<int16_t> scaled_;
    std::vector<int16_t> boosted_;
    std::vector<int16_t> silence_;
};

// A word of the text being spoken: where it is in the fragment's source text, for SPEI_WORD_BOUNDARY.
struct Word {
    ULONG pos;
    ULONG len;
};

// The text for the library, with an index mark before each blank-separated word ("[:in 7]word"). The library's
// mark fix (ENGINE_FIX_MARKS, REFERENCE.md s17.13) leaves the speech as it is without them, and reports each mark
// at the first sample of its word's first phone. Marks are numbered from `next` (1-32767, wrapping); each number's
// word goes into `words`.
std::string with_marks(const std::wstring& text, ULONG src_offset, int& next, std::map<uint32_t, Word>& words)
{
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = i;
        while (j < text.size() && iswspace(text[j])) {
            ++j;
        }
        out += dectalk::utils::wstring_to_string(text.substr(i, j - i));
        if (j == text.size()) {
            break;
        }
        size_t k = j;
        while (k < text.size() && !iswspace(text[k])) {
            ++k;
        }
        const int id = next;
        next = next >= 32767 ? 1 : next + 1;
        words[static_cast<uint32_t>(id)] = {src_offset + static_cast<ULONG>(j), static_cast<ULONG>(k - j)};
        out += "[:in " + std::to_string(id) + "]";
        out += dectalk::utils::wstring_to_string(text.substr(j, k - j));
        i = k;
    }
    // Control characters would be spoken or act as commands; the text pipeline takes blanks.
    for (char& c : out) {
        if (c == '\r' || c == '\n' || c == '\t' || c == '\v' || c == '\f') {
            c = ' ';
        }
    }
    return out;
}

enum class PieceEnd { Spoken, Stuck, Aborted };

// Until the first sound of a Speak call, silence is only latency: the host is waiting on it. The clause start, and
// in v2.0 the comma-clause its [:ra] and voice names begin, put 75-300 ms of it ahead of the first word. It is
// dropped, but for the last 10 ms before the sound, and marks in it move to the sound's start.
class LeadTrim {
public:
    // Trims `block` while no sound has come; returns how many samples were dropped from its front (marks shift by
    // that much). ponytail: the 10 ms come from the sound's own block only (less when it starts in the first 10 ms).
    size_t trim(std::vector<int16_t>& block)
    {
        if (done_) {
            return 0;
        }
        size_t i = 0;
        while (i < block.size() && std::abs(block[i]) <= kQuiet) {
            ++i;
        }
        if (i == block.size()) {
            block.clear();
            return i;
        }
        done_ = true;
        const size_t drop = i > kKeep ? i - kKeep : 0;
        block.erase(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(drop));
        return drop;
    }

private:
    static constexpr int kQuiet = 160;          // the DAC's idle wander, a few 12-bit steps
    static constexpr size_t kKeep = 100;        // 10 ms
    bool done_ = false;
};

// Runs the library until it has said everything queued, writing the audio to the host and a word boundary at each
// mark. Honours host actions every block. Caller holds the library's mutex.
PieceEnd pump(Library& lib, ISpTTSEngineSite* site, SiteWriter& out, Loudness& loudness, long& sapi_rate,
              const std::map<uint32_t, Word>& words, bool want_word, LeadTrim& lead)
{
    std::vector<int16_t> block;
    std::vector<std::pair<uint32_t, size_t>> marks;
    size_t produced = 0;
    for (;;) {
        const DWORD actions = site->GetActions();
        if (actions & SPVES_ABORT) {
            return PieceEnd::Aborted;
        }
        if (actions & SPVES_RATE) {
            site->GetRate(&sapi_rate);
        }
        if (actions & SPVES_VOLUME) {
            site->GetVolume(&loudness.host);
            out.set_volume(loudness.percent());
        }
        if (produced >= kPieceCapSamples) {
            return PieceEnd::Stuck;
        }
        const size_t got = lib.run(block, kBlockSamples, marks);
        produced += got;
        const size_t dropped = lead.trim(block);
        if (want_word) {
            for (const auto& [id, at] : marks) {
                const auto w = words.find(id);
                if (w != words.end()) {
                    add_event(site, SPEI_WORD_BOUNDARY, out.offset_of(at > dropped ? at - dropped : 0),
                              w->second.len, static_cast<LPARAM>(w->second.pos), SPET_LPARAM_IS_UNDEFINED);
                }
            }
        }
        if (!block.empty() && !out.write(block.data(), block.size())) {
            return PieceEnd::Aborted;
        }
        if (got < kBlockSamples) {
            return PieceEnd::Spoken;
        }
    }
}

}  // namespace

DectalkTtsEngine::DectalkTtsEngine() = default;

DectalkTtsEngine::~DectalkTtsEngine() = default;

STDMETHODIMP DectalkTtsEngine::SetObjectToken(ISpObjectToken* pToken)
{
    if (!pToken) {
        return E_INVALIDARG;
    }

    try {
        ISpDataKeyPtr attr;
        if (FAILED(pToken->OpenKey(L"Attributes", &attr)) || !attr) {
            return E_INVALIDARG;
        }

        dectalk::utils::out_ptr<wchar_t> voice_val(CoTaskMemFree);
        dectalk::utils::out_ptr<wchar_t> firmware_val(CoTaskMemFree);
        if (FAILED(attr->GetStringValue(L"DtcVoice", voice_val.address())) || !voice_val.get() ||
            FAILED(attr->GetStringValue(L"DtcFirmware", firmware_val.address())) || !firmware_val.get()) {
            return E_INVALIDARG;
        }

        const std::string voice_key = dectalk::utils::wstring_to_string(voice_val.get());
        const std::string firmware  = dectalk::utils::wstring_to_string(firmware_val.get());

        const dtc01::VoiceDef* voice = dtc01::find_voice(voice_key, firmware);
        if (!voice) {
            return E_INVALIDARG;
        }

        // Speak() reads these under the same lock (ThreadingModel=Both: a host may race the two).
        std::lock_guard<std::mutex> lk(mu_);
        voice_key_ = voice_key;
        firmware_  = firmware;
        mnemonic_  = voice->mnemonic;
        voice_resolved_ = true;
        token_ = pToken;
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP DectalkTtsEngine::GetObjectToken(ISpObjectToken** ppToken)
{
    if (!ppToken) {
        return E_POINTER;
    }
    *ppToken = nullptr;
    if (!token_) {
        return E_UNEXPECTED;
    }
    token_.AddRef();
    *ppToken = token_.GetInterfacePtr();
    return S_OK;
}

STDMETHODIMP DectalkTtsEngine::GetOutputFormat(
    const GUID* /*pTargetFmtId*/,
    const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
    GUID* pOutputFormatId,
    WAVEFORMATEX** ppCoMemOutputWaveFormatEx)
{
    if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) {
        return E_POINTER;
    }
    *pOutputFormatId = SPDFID_WaveFormatEx;
    *ppCoMemOutputWaveFormatEx = nullptr;

    auto* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!wfx) {
        return E_OUTOFMEMORY;
    }

    // Fixed format: the library's own, 10 kHz (it never resamples).
    wfx->wFormatTag      = AUDIO_FORMAT_TAG;
    wfx->nChannels       = AUDIO_CHANNELS;
    wfx->nSamplesPerSec  = AUDIO_SAMPLE_RATE;
    wfx->wBitsPerSample  = AUDIO_BITS;
    wfx->nBlockAlign     = static_cast<WORD>(wfx->nChannels * wfx->wBitsPerSample / 8);
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    wfx->cbSize          = 0;

    *ppCoMemOutputWaveFormatEx = wfx;
    return S_OK;
}

STDMETHODIMP DectalkTtsEngine::Speak(
    DWORD /*dwSpeakFlags*/,
    REFGUID /*rguidFormatId*/,
    const WAVEFORMATEX* /*pWaveFormatEx*/,
    const SPVTEXTFRAG* pTextFragList,
    ISpTTSEngineSite* pOutputSite)
{
    if (!pTextFragList || !pOutputSite) {
        return E_INVALIDARG;
    }

    try {
        // First, so turning the diagnostic log on or off in the configuration utility takes effect with this
        // utterance rather than when the host restarts.
        DebugLog::RefreshEnabled();

        std::string voice_key, firmware, mnemonic;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!voice_resolved_) {
                return E_FAIL;
            }
            voice_key = voice_key_;
            firmware = firmware_;
            mnemonic = mnemonic_;
        }
        Library* lib = Library::get(firmware);
        if (!lib) {
            return E_FAIL;              // DECtalk.dll / DECtalk20.dll missing beside this DLL, or it did not start
        }
        // One utterance at a time per firmware: the library's engine is one per process (and per DLL copy).
        std::lock_guard<std::mutex> lk(lib->mutex());

        // HKCU settings, read once per Speak call so a configuration-utility change takes effect on the next
        // utterance.
        const dectalk::settings::GlobalSettings global = dectalk::settings::load_global();
        const dectalk::settings::VoiceSettings voice_cfg =
            dectalk::settings::load_voice(voice_key.c_str(), firmware.c_str());

        // 1:1 slider copy (VoiceSettings -> DvParams); a slider still at 50 (this voice's own default) is left out.
        dtc01::DvParams dvp;
        dvp.inflection       = voice_cfg.inflection;
        dvp.head_size        = voice_cfg.head_size;
        dvp.breathiness      = voice_cfg.breathiness;
        dvp.richness         = voice_cfg.richness;
        dvp.smoothness       = voice_cfg.smoothness;
        dvp.loudness         = voice_cfg.loudness;
        dvp.laryngealization = voice_cfg.laryngealization;
        dvp.assertiveness    = voice_cfg.assertiveness;
        dvp.pitch            = voice_cfg.pitch;

        // RateBoost: a percent of extra time compression on top of the rate (0 -> 1.0, 100 -> 2.0).
        const double rate_boost_factor =
            1.0 + std::clamp(global.rate_boost,
                              dectalk::settings::RATE_BOOST_MIN,
                              dectalk::settings::RATE_BOOST_MAX) / 100.0;

        ULONGLONG event_interest = 0;
        pOutputSite->GetEventInterest(&event_interest);
        const bool want_word     = (event_interest & SPFEI(SPEI_WORD_BOUNDARY)) != 0;
        const bool want_sentence = (event_interest & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;
        const bool want_bookmark = (event_interest & SPFEI(SPEI_TTS_BOOKMARK)) != 0;

        // Rate, pitch and volume are the host's (ISpVoice::SetRate/SetVolume, and markup such as <rate absspeed>,
        // <volume level>, <pitch absmiddle>) unless "Allow SAPI5 apps to control rate, pitch and volume" is
        // unticked; then the configuration utility's RatePercent, VolumeDB and this voice's Pitch slider decide.
        const bool app_control = global.app_control;

        long sapi_rate = 0;
        pOutputSite->GetRate(&sapi_rate);
        Loudness loudness;
        loudness.app_control = app_control;
        loudness.volume_db = global.volume_db;
        pOutputSite->GetVolume(&loudness.host);

        // The voice is selected at the first spoken fragment of each call: the library keeps the last call's
        // voice, rate and [:dv] values, and this puts them back to the voice's own. A [:dv] value then sticks
        // until the voice is selected again, so each fragment sends only what differs from what is held.
        std::string current_mnemonic;
        const dtc01::DvValues voice_defaults = dtc01::dv_defaults(voice_key);
        dtc01::DvValues held = voice_defaults;

        SiteWriter out(pOutputSite, rate_boost_factor);
        bool aborted = false;
        std::map<uint32_t, Word> words;
        int next_mark = 1;
        int current_wpm = 0;            // the rate last sent in this call (v2.0 starts a clause at each [:ra])
        LeadTrim lead;

        for (const SPVTEXTFRAG* frag = pTextFragList; frag && !aborted; frag = frag->pNext) {
            const DWORD actions = pOutputSite->GetActions();
            if (actions & SPVES_ABORT) {
                aborted = true;
                break;
            }
            if (actions & SPVES_RATE) {
                pOutputSite->GetRate(&sapi_rate);
            }
            if (actions & SPVES_VOLUME) {
                pOutputSite->GetVolume(&loudness.host);
            }

            switch (frag->State.eAction) {
            case SPVA_Bookmark: {
                if (!want_bookmark) {
                    break;
                }
                const std::wstring mark = (frag->ulTextLen && frag->pTextStart)
                    ? std::wstring(frag->pTextStart, frag->ulTextLen) : std::wstring();
                const long id = mark.empty() ? 0 : _wtol(mark.c_str());
                add_event(pOutputSite, SPEI_TTS_BOOKMARK, out.bytes(),
                          static_cast<WPARAM>(id),
                          reinterpret_cast<LPARAM>(mark.c_str()),
                          mark.empty() ? SPET_LPARAM_IS_UNDEFINED : SPET_LPARAM_IS_STRING);
                break;
            }

            case SPVA_Silence: {
                const size_t samples =
                    static_cast<size_t>(AUDIO_SAMPLE_RATE) * frag->State.SilenceMSecs / 1000u;
                if (!out.write_silence(samples)) {
                    aborted = true;
                }
                break;
            }

            case SPVA_Speak:
            case SPVA_SpellOut:
            case SPVA_Pronounce: {
                if (frag->ulTextLen == 0 || !frag->pTextStart) {
                    break;
                }
                // SPVA_SpellOut: the text is spoken as it is. sanitize_text() only replaces an ASCII character with
                // a space, so the sanitized text lines up with the fragment unit for unit.
                const std::wstring text = dectalk::utils::string_to_wstring(dtc01::sanitize_text(
                    dectalk::utils::wstring_to_string(std::wstring(frag->pTextStart, frag->ulTextLen))));

                int wpm = 0;
                dtc01::DvParams sliders = dvp;
                if (app_control) {
                    const int frag_rate = std::clamp<int>(
                        static_cast<int>(sapi_rate) + frag->State.RateAdj, SAPI_RATE_MIN, SAPI_RATE_MAX);
                    wpm = sapi_rate_to_wpm(frag_rate);
                    // SAPI pitch is -10..+10 by convention; NVDA sends its pitch setting as percent / 2 - 25, and
                    // more for capital letters. Two slider steps per unit lands NVDA's setting on this voice's own
                    // Pitch slider: 50 is the voice's own pitch, 0 and 100 the lowest and highest it will make.
                    constexpr int kPitchStepsPerUnit = 2;
                    sliders.pitch = std::clamp(
                        dectalk::settings::SLIDER_DEF +
                            kPitchStepsPerUnit * static_cast<int>(frag->State.PitchAdj.MiddleAdj),
                        dectalk::settings::SLIDER_MIN, dectalk::settings::SLIDER_MAX);
                } else {
                    // RatePercent of the default rate; rate_command() clamps to [120, 350].
                    wpm = static_cast<int>(std::lround(DEFAULT_WPM * global.rate_percent / 100.0));
                }
                loudness.fragment = frag->State.Volume;
                out.set_volume(loudness.percent());

                const dtc01::DvValues wanted = dtc01::dv_values(voice_key, sliders);
                bool select_voice = current_mnemonic != mnemonic;
                bool needs_voice = false;
                std::string dv = dtc01::dv_change_command(select_voice ? voice_defaults : held,
                                                          wanted, &needs_voice);
                if (needs_voice) {
                    // A default [:dv] refuses (Kit's pitch, 306) comes back only by selecting the voice again.
                    select_voice = true;
                    dv = dtc01::dv_change_command(voice_defaults, wanted, nullptr);
                }

                std::string fed;
                if (select_voice) {
                    fed += dtc01::voice_command(mnemonic.c_str());
                    fed += " ";
                }
                if (select_voice || wpm != current_wpm) {
                    fed += dtc01::rate_command(wpm);
                    current_wpm = wpm;
                }
                fed += dv;
                current_mnemonic = mnemonic;
                held = wanted;
                fed += with_marks(text, frag->ulTextSrcOffset, next_mark, words);

                if (want_sentence) {
                    add_event(pOutputSite, SPEI_SENTENCE_BOUNDARY, out.bytes(),
                              frag->ulTextLen, frag->ulTextSrcOffset, SPET_LPARAM_IS_UNDEFINED);
                }

                DECTALK_LOG("DectalkTtsEngine: Speak voice=%s firmware=%s wpm=%d pitch=%d (host %ld) volume=%d "
                            "fed=[%s]", voice_key.c_str(), firmware.c_str(), wpm, sliders.pitch,
                            static_cast<long>(frag->State.PitchAdj.MiddleAdj), loudness.percent(), fed.c_str());

                if (!lib->speak(fed)) {
                    break;
                }
                const PieceEnd end = pump(*lib, pOutputSite, out, loudness, sapi_rate, words, want_word, lead);
                if (end == PieceEnd::Aborted) {
                    aborted = true;
                    break;
                }
                if (end == PieceEnd::Stuck) {
                    lib->stop();
                }
                if (!out.finish_fragment()) {
                    aborted = true;
                }
                break;
            }

            default:
                break;  // SPVA_ParseUnknown and friends: nothing to speak.
            }
        }

        if (aborted) {
            lib->stop();                // the rest of the text and its audio are dropped (DT_STOP)
        }

        DECTALK_LOG("DectalkTtsEngine: Speak done, aborted=%d, %llu audio bytes written",
                    aborted ? 1 : 0, static_cast<unsigned long long>(out.bytes()));
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

}  // namespace sapi
}  // namespace dtc01
