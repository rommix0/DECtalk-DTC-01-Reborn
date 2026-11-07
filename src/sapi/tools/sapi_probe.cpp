// sapi_probe.cpp - end-to-end COM speech probe for DectalkDtc01SAPI.dll
// (Phase B deliverable, Task B4).
//
// Loads DectalkDtc01SAPI.dll, creates the real ISpTTSEngine coclass through
// DllGetClassObject (NO regsvr32, no registration of the DLL itself, no admin
// rights needed), hands it a token exposing DtcVoice/DtcFirmware, and drives
// one Speak() call with a stand-in ISpTTSEngineSite that captures the PCM to
// a WAV file. That exercises exactly the code path a SAPI5 host (e.g. NVDA,
// Narrator) takes when it speaks through this driver.
//
// Adapted from BstSpeech-sapi-master/tools/sapi_probe.cpp: the token this
// engine's SetObjectToken (sapi/src/DectalkTtsEngine.cpp) reads is
// DtcVoice/DtcFirmware rather than BstSpeech's BstEngine/BstVoice, and
// SetObjectToken never calls anything on the token besides
// OpenKey(L"Attributes") + GetStringValue on those two names -- so, exactly
// like the BstSpeech template, an in-memory stand-in ISpObjectToken/
// ISpDataKey pair satisfies the real contract with no registry involved at
// all (see the task-B4 report for why this is preferred over writing a
// throwaway HKCU tree).
//
//   sapi_probe <DectalkDtc01SAPI.dll> <voice_key> <firmware> <out.wav> <text>
//
// The engine finds DECtalk.dll / DECtalk20.dll beside the SAPI DLL. The probe
// also checks the word boundaries: one per blank-separated word, in order.

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// --- a data key holding just the attributes SetObjectToken reads -----------
class ProbeDataKey : public ISpDataKey
{
public:
    ProbeDataKey(const std::wstring& voice, const std::wstring& firmware)
        : voice_(voice), firmware_(firmware) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(ISpDataKey)) {
            *ppv = static_cast<ISpDataKey*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override
    {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHOD(GetStringValue)(LPCWSTR name, LPWSTR* value) override
    {
        const wchar_t* found = nullptr;
        if (name && _wcsicmp(name, L"DtcVoice") == 0) found = voice_.c_str();
        else if (name && _wcsicmp(name, L"DtcFirmware") == 0) found = firmware_.c_str();
        if (!found) return SPERR_NOT_FOUND;

        const size_t bytes = (wcslen(found) + 1) * sizeof(wchar_t);
        *value = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
        if (!*value) return E_OUTOFMEMORY;
        memcpy(*value, found, bytes);
        return S_OK;
    }

    STDMETHOD(SetData)(LPCWSTR, ULONG, const BYTE*) override { return E_NOTIMPL; }
    STDMETHOD(GetData)(LPCWSTR, ULONG*, BYTE*) override { return E_NOTIMPL; }
    STDMETHOD(SetStringValue)(LPCWSTR, LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(SetDWORD)(LPCWSTR, DWORD) override { return E_NOTIMPL; }
    STDMETHOD(GetDWORD)(LPCWSTR, DWORD*) override { return E_NOTIMPL; }
    STDMETHOD(OpenKey)(LPCWSTR, ISpDataKey**) override { return SPERR_NOT_FOUND; }
    STDMETHOD(CreateKey)(LPCWSTR, ISpDataKey**) override { return E_NOTIMPL; }
    STDMETHOD(DeleteKey)(LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(DeleteValue)(LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(EnumKeys)(ULONG, LPWSTR*) override { return SPERR_NO_MORE_ITEMS; }
    STDMETHOD(EnumValues)(ULONG, LPWSTR*) override { return SPERR_NO_MORE_ITEMS; }

private:
    LONG ref_ = 1;
    std::wstring voice_, firmware_;
};

// --- a token whose only job is to hand back that attributes key ------------
class ProbeToken : public ISpObjectToken
{
public:
    ProbeToken(const std::wstring& voice, const std::wstring& firmware)
        : voice_(voice), firmware_(firmware) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(ISpDataKey) ||
            riid == __uuidof(ISpObjectToken)) {
            *ppv = static_cast<ISpObjectToken*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override
    {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHOD(OpenKey)(LPCWSTR name, ISpDataKey** key) override
    {
        if (!name || _wcsicmp(name, L"Attributes") != 0) return SPERR_NOT_FOUND;
        *key = new ProbeDataKey(voice_, firmware_);
        return S_OK;
    }

    STDMETHOD(SetData)(LPCWSTR, ULONG, const BYTE*) override { return E_NOTIMPL; }
    STDMETHOD(GetData)(LPCWSTR, ULONG*, BYTE*) override { return E_NOTIMPL; }
    STDMETHOD(SetStringValue)(LPCWSTR, LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(GetStringValue)(LPCWSTR, LPWSTR*) override { return SPERR_NOT_FOUND; }
    STDMETHOD(SetDWORD)(LPCWSTR, DWORD) override { return E_NOTIMPL; }
    STDMETHOD(GetDWORD)(LPCWSTR, DWORD*) override { return E_NOTIMPL; }
    STDMETHOD(CreateKey)(LPCWSTR, ISpDataKey**) override { return E_NOTIMPL; }
    STDMETHOD(DeleteKey)(LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(DeleteValue)(LPCWSTR) override { return E_NOTIMPL; }
    STDMETHOD(EnumKeys)(ULONG, LPWSTR*) override { return SPERR_NO_MORE_ITEMS; }
    STDMETHOD(EnumValues)(ULONG, LPWSTR*) override { return SPERR_NO_MORE_ITEMS; }

    STDMETHOD(SetId)(LPCWSTR, LPCWSTR, BOOL) override { return E_NOTIMPL; }
    STDMETHOD(GetId)(LPWSTR*) override { return E_NOTIMPL; }
    STDMETHOD(GetCategory)(ISpObjectTokenCategory**) override { return E_NOTIMPL; }
    STDMETHOD(CreateInstance)(IUnknown*, DWORD, REFIID, void**) override { return E_NOTIMPL; }
    STDMETHOD(GetStorageFileName)(REFCLSID, LPCWSTR, LPCWSTR, ULONG, LPWSTR*) override { return E_NOTIMPL; }
    STDMETHOD(RemoveStorageFileName)(REFCLSID, LPCWSTR, BOOL) override { return E_NOTIMPL; }
    STDMETHOD(Remove)(const CLSID*) override { return E_NOTIMPL; }
    STDMETHOD(IsUISupported)(LPCWSTR, void*, ULONG, IUnknown*, BOOL*) override { return E_NOTIMPL; }
    STDMETHOD(DisplayUI)(HWND, LPCWSTR, LPCWSTR, void*, ULONG, IUnknown*) override { return E_NOTIMPL; }
    STDMETHOD(MatchesAttributes)(LPCWSTR, BOOL*) override { return E_NOTIMPL; }

private:
    LONG ref_ = 1;
    std::wstring voice_, firmware_;
};

// --- the site the engine writes its audio and events into -------------------
class ProbeSite : public ISpTTSEngineSite
{
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(ISpEventSink) ||
            riid == __uuidof(ISpTTSEngineSite)) {
            *ppv = static_cast<ISpTTSEngineSite*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return InterlockedIncrement(&ref_); }
    STDMETHOD_(ULONG, Release)() override
    {
        const LONG n = InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    // ISpEventSink -- word boundaries (from the library's index marks), kept to be checked
    STDMETHOD(AddEvents)(const SPEVENT* ev, ULONG n) override
    {
        for (ULONG i = 0; i < n; ++i) {
            if (ev[i].eEventId == SPEI_WORD_BOUNDARY) {
                words_.push_back({ev[i].ullAudioStreamOffset, static_cast<ULONG>(ev[i].lParam),
                                  static_cast<ULONG>(ev[i].wParam)});
            }
        }
        return S_OK;
    }
    STDMETHOD(GetEventInterest)(ULONGLONG* interest) override
    {
        *interest = SPFEI(SPEI_WORD_BOUNDARY);
        return S_OK;
    }
    struct WordEvent { ULONGLONG offset; ULONG pos, len; };
    const std::vector<WordEvent>& words() const { return words_; }

    // ISpTTSEngineSite
    STDMETHOD_(DWORD, GetActions)() override { return SPVES_CONTINUE; }
    STDMETHOD(Write)(const void* data, ULONG count, ULONG* written) override
    {
        const auto* p = static_cast<const BYTE*>(data);
        pcm_.insert(pcm_.end(), p, p + count);
        if (written) *written = count;
        return S_OK;
    }
    STDMETHOD(GetRate)(long* rate) override { *rate = 0; return S_OK; }
    STDMETHOD(GetVolume)(USHORT* volume) override { *volume = 100; return S_OK; }
    STDMETHOD(GetSkipInfo)(SPVSKIPTYPE* type, long* items) override
    {
        *type = SPVST_SENTENCE;
        *items = 0;
        return S_OK;
    }
    STDMETHOD(CompleteSkip)(long) override { return S_OK; }

    const std::vector<BYTE>& pcm() const { return pcm_; }

private:
    LONG ref_ = 1;
    std::vector<BYTE> pcm_;
    std::vector<WordEvent> words_;
};

bool write_wav(const wchar_t* path, const std::vector<BYTE>& pcm, DWORD rate)
{
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;

    const DWORD data_size = static_cast<DWORD>(pcm.size());
    const DWORD byte_rate = rate * 2;
    fwrite("RIFF", 1, 4, f);
    DWORD riff = 36 + data_size; fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    DWORD fmt_size = 16;      fwrite(&fmt_size, 4, 1, f);
    WORD fmt = 1;             fwrite(&fmt, 2, 1, f);
    WORD ch = 1;              fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    WORD align = 2;           fwrite(&align, 2, 1, f);
    WORD bits = 16;           fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);  fwrite(&data_size, 4, 1, f);
    if (data_size) fwrite(pcm.data(), 1, data_size, f);
    fclose(f);
    return true;
}

// CLSID of dtc01::sapi::DectalkTtsEngine (sapi/src/DectalkTtsEngine.hpp).
// Hardcoded here (rather than pulling in DectalkTtsEngine.hpp/dtc01common)
// so this probe only needs DllGetClassObject from the built DLL, exactly
// like a real SAPI host that never links against the engine's sources.
const CLSID CLSID_DectalkTtsEngine =
    { 0xE877CE12, 0xF153, 0x40DF, { 0xAE, 0xAC, 0x2A, 0xFA, 0x4D, 0x4A, 0x3D, 0xE8 } };

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 6) {
        wprintf(L"usage: sapi_probe <DectalkDtc01SAPI.dll> <voice_key> <firmware> "
                L"<out.wav> <text>\n");
        return 2;
    }

    const wchar_t* dll_path = argv[1];
    const std::wstring voice_key = argv[2];
    const std::wstring firmware = argv[3];
    const wchar_t* wav_path = argv[4];
    const std::wstring text = argv[5];

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrInit)) {
        wprintf(L"FAIL: CoInitializeEx -> 0x%08X\n", hrInit);
        return 1;
    }

    HMODULE mod = LoadLibraryW(dll_path);
    if (!mod) {
        wprintf(L"FAIL: LoadLibrary(%s) -> %lu\n", dll_path, GetLastError());
        CoUninitialize();
        return 1;
    }
    auto get_class = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(mod, "DllGetClassObject"));
    if (!get_class) {
        wprintf(L"FAIL: DllGetClassObject not exported\n");
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    IClassFactory* factory = nullptr;
    HRESULT hr = get_class(CLSID_DectalkTtsEngine, IID_IClassFactory, (void**)&factory);
    if (FAILED(hr) || !factory) {
        wprintf(L"FAIL: DllGetClassObject -> 0x%08X\n", hr);
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    ISpTTSEngine* engine = nullptr;
    hr = factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine), (void**)&engine);
    factory->Release();
    if (FAILED(hr) || !engine) {
        wprintf(L"FAIL: CreateInstance(ISpTTSEngine) -> 0x%08X\n", hr);
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    ISpObjectWithToken* with_token = nullptr;
    hr = engine->QueryInterface(__uuidof(ISpObjectWithToken), (void**)&with_token);
    if (FAILED(hr) || !with_token) {
        wprintf(L"FAIL: QueryInterface(ISpObjectWithToken) -> 0x%08X\n", hr);
        engine->Release();
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    auto* token = new ProbeToken(voice_key, firmware);
    hr = with_token->SetObjectToken(token);
    if (FAILED(hr)) {
        wprintf(L"FAIL: SetObjectToken(%s/%s) -> 0x%08X\n", voice_key.c_str(), firmware.c_str(), hr);
        token->Release();
        with_token->Release();
        engine->Release();
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    GUID fmt_id = {};
    WAVEFORMATEX* wfx = nullptr;
    hr = engine->GetOutputFormat(&SPDFID_WaveFormatEx, nullptr, &fmt_id, &wfx);
    if (FAILED(hr) || !wfx) {
        wprintf(L"FAIL: GetOutputFormat -> 0x%08X\n", hr);
        token->Release();
        with_token->Release();
        engine->Release();
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }
    wprintf(L"GetOutputFormat: tag=%u channels=%u rate=%lu bits=%u block=%u avg=%lu (want 1/1/10000/16)\n",
            wfx->wFormatTag, wfx->nChannels, wfx->nSamplesPerSec, wfx->wBitsPerSample,
            wfx->nBlockAlign, wfx->nAvgBytesPerSec);
    const bool fmt_ok = wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->nChannels == 1 &&
                        wfx->nSamplesPerSec == 10000 && wfx->wBitsPerSample == 16;
    const DWORD sample_rate = wfx->nSamplesPerSec;

    SPVTEXTFRAG frag = {};
    frag.pNext = nullptr;
    frag.State.eAction = SPVA_Speak;
    frag.State.LangID = 0x409;
    frag.State.EmphAdj = 0;
    frag.State.RateAdj = 0;
    frag.State.Volume = 100;
    frag.State.PitchAdj.MiddleAdj = 0;
    frag.State.PitchAdj.RangeAdj = 0;
    frag.State.SilenceMSecs = 0;
    frag.pTextStart = text.c_str();
    frag.ulTextLen = static_cast<ULONG>(text.size());
    frag.ulTextSrcOffset = 0;

    auto* site = new ProbeSite();
    hr = engine->Speak(SPF_DEFAULT, SPDFID_WaveFormatEx, wfx, &frag, site);

    const bool wrote = write_wav(wav_path, site->pcm(), sample_rate);

    wprintf(L"voice %s/%s | Speak -> 0x%08X | %lu bytes pcm | wav %s\n",
            voice_key.c_str(), firmware.c_str(), hr,
            static_cast<unsigned long>(site->pcm().size()),
            wrote ? L"written" : L"FAILED TO WRITE");

    // One word boundary per blank-separated word, in order, at increasing offsets inside the audio.
    size_t expected = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (!iswspace(text[i]) && (i == 0 || iswspace(text[i - 1]))) ++expected;
    }
    bool words_ok = site->words().size() == expected;
    ULONGLONG last = 0;
    size_t at = 0;
    for (const auto& w : site->words()) {
        while (at < text.size() && iswspace(text[at])) ++at;
        words_ok = words_ok && w.pos == at && w.offset >= last && w.offset < site->pcm().size();
        wprintf(L"  word %6.0f ms  %.*s\n", w.offset / 20.0, static_cast<int>(w.len), text.c_str() + w.pos);
        last = w.offset;
        at = w.pos + w.len;
    }
    wprintf(L"word boundaries: %zu of %zu words%s\n", site->words().size(), expected, words_ok ? L"" : L" -- FAIL");

    const bool ok = SUCCEEDED(hr) && fmt_ok && wrote && !site->pcm().empty() && words_ok;

    site->Release();
    CoTaskMemFree(wfx);
    with_token->Release();
    engine->Release();
    token->Release();
    FreeLibrary(mod);
    CoUninitialize();
    return ok ? 0 : 1;
}
