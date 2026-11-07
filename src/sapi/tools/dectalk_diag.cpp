// dectalk_diag.cpp - DectalkDiagnostics.exe (Task D2).
//
// A console diagnostics tool that drives the REAL DectalkDtc01SAPI.dll
// through DllGetClassObject (no regsvr32, no registration, no admin rights)
// for all 18 voice tokens and reports pass/fail for each. This is what a
// user (or a support request) runs after install to confirm every voice
// actually renders audio through the shipped DLL.
//
// Modeled on sapi_probe.cpp: reuses its in-memory ProbeToken/ProbeDataKey
// stand-in for ISpObjectToken/ISpDataKey (the engine's SetObjectToken only
// ever reads Attributes\DtcVoice and Attributes\DtcFirmware, so no registry
// involvement is needed) and its ProbeSite stand-in for ISpTTSEngineSite,
// plus the same hardcoded CLSID_DectalkTtsEngine literal -- this tool loads
// the DLL exactly like sapi_probe.cpp and exactly like a real SAPI host,
// never linking against the engine's own sources for that part.
//
// Unlike sapi_probe.cpp, this tool links dtc01common so it can iterate the
// canonical 18-entry dtc01::VOICES table (sapi/src/voices.hpp) instead of
// taking one voice per invocation on the command line.
//
//   DectalkDiagnostics.exe [path-to-DectalkDtc01SAPI.dll]
//
// DLL resolution: the CLI arg if given, else DectalkDtc01SAPI.dll beside
// this exe, else in the current directory.
//
// The engine speaks through the speech library beside the SAPI DLL: DECtalk.dll
// (v1.8) and DECtalk20.dll (v2.0); the report header says which are there.
//
// Report: printed to stdout AND written to
// %LOCALAPPDATA%\DECtalkDTC01\diagnostics-report.txt (falling back to the
// exe's own directory if LOCALAPPDATA is unavailable).
//
// Exit code: 0 iff every voice produced non-empty audio with a success
// HRESULT and the expected 1/1/10000/16 format; non-zero otherwise -- so
// this doubles as an install-time smoke test.

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "voices.hpp"

namespace {

// --- a data key holding just the attributes SetObjectToken reads -----------
// (identical contract to sapi_probe.cpp's ProbeDataKey -- see that file's
// header comment for why an in-memory stand-in satisfies the real one.)
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

    // ISpEventSink -- no boundary/bookmark events wanted; this tool only
    // needs audio bytes and the HRESULT.
    STDMETHOD(AddEvents)(const SPEVENT*, ULONG) override { return S_OK; }
    STDMETHOD(GetEventInterest)(ULONGLONG* interest) override { *interest = 0; return S_OK; }

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
};

// CLSID of dtc01::sapi::DectalkTtsEngine (sapi/src/DectalkTtsEngine.hpp).
// Hardcoded here (rather than pulling in DectalkTtsEngine.hpp) so this tool
// only needs DllGetClassObject from the built DLL, exactly like a real SAPI
// host that never links against the engine's sources -- same rationale as
// sapi_probe.cpp.
const CLSID CLSID_DectalkTtsEngine =
    { 0xE877CE12, 0xF153, 0x40DF, { 0xAE, 0xAC, 0x2A, 0xFA, 0x4D, 0x4A, 0x3D, 0xE8 } };

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

const wchar_t* const kTestText = L"DECtalk D T C 01 voice test.";

#ifdef BUILD_X64
constexpr const wchar_t* kBitness = L"x64 (64-bit)";
#else
constexpr const wchar_t* kBitness = L"x86 (32-bit)";
#endif

std::wstring env_var(const wchar_t* name)
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return buf;
}

// DLL resolution per the brief: CLI arg first, else beside this exe, else CWD.
std::wstring resolve_dll_path(int argc, wchar_t** argv)
{
    if (argc > 1 && argv[1][0] != L'\0') {
        return argv[1];
    }

    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) != 0) {
        std::wstring path(exe_path);
        const size_t slash = path.find_last_of(L"\\/");
        std::wstring dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
        std::wstring beside = dir + L"\\DectalkDtc01SAPI.dll";
        if (GetFileAttributesW(beside.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return beside;
        }
    }

    return L"DectalkDtc01SAPI.dll";  // let LoadLibrary search the CWD/PATH
}

// One voice's outcome.
struct VoiceResult {
    const dtc01::VoiceDef* voice = nullptr;
    HRESULT hr_set_token = S_OK;
    HRESULT hr_format = S_OK;
    HRESULT hr_speak = S_OK;
    bool format_ok = false;
    size_t pcm_bytes = 0;
    double duration_sec = 0.0;
    bool pass = false;
};

VoiceResult run_voice(HMODULE mod, DllGetClassObjectFn get_class, const dtc01::VoiceDef& voice)
{
    VoiceResult result;
    result.voice = &voice;

    IClassFactory* factory = nullptr;
    HRESULT hr = get_class(CLSID_DectalkTtsEngine, IID_IClassFactory, (void**)&factory);
    if (FAILED(hr) || !factory) {
        result.hr_speak = hr;
        return result;
    }

    ISpTTSEngine* engine = nullptr;
    hr = factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine), (void**)&engine);
    factory->Release();
    if (FAILED(hr) || !engine) {
        result.hr_speak = hr;
        return result;
    }

    ISpObjectWithToken* with_token = nullptr;
    hr = engine->QueryInterface(__uuidof(ISpObjectWithToken), (void**)&with_token);
    if (FAILED(hr) || !with_token) {
        result.hr_speak = hr;
        engine->Release();
        return result;
    }

    std::wstring voice_key(voice.key, voice.key + strlen(voice.key));
    std::wstring firmware(voice.firmware, voice.firmware + strlen(voice.firmware));
    auto* token = new ProbeToken(voice_key, firmware);
    hr = with_token->SetObjectToken(token);
    result.hr_set_token = hr;
    if (FAILED(hr)) {
        token->Release();
        with_token->Release();
        engine->Release();
        return result;
    }

    GUID fmt_id = {};
    WAVEFORMATEX* wfx = nullptr;
    hr = engine->GetOutputFormat(&SPDFID_WaveFormatEx, nullptr, &fmt_id, &wfx);
    result.hr_format = hr;
    if (FAILED(hr) || !wfx) {
        token->Release();
        with_token->Release();
        engine->Release();
        return result;
    }
    result.format_ok = wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->nChannels == 1 &&
                        wfx->nSamplesPerSec == 10000 && wfx->wBitsPerSample == 16;

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
    frag.pTextStart = kTestText;
    frag.ulTextLen = static_cast<ULONG>(wcslen(kTestText));
    frag.ulTextSrcOffset = 0;

    auto* site = new ProbeSite();
    hr = engine->Speak(SPF_DEFAULT, SPDFID_WaveFormatEx, wfx, &frag, site);
    result.hr_speak = hr;
    result.pcm_bytes = site->pcm().size();
    result.duration_sec = static_cast<double>(result.pcm_bytes) / (10000.0 * 2.0);

    result.pass = SUCCEEDED(hr) && result.format_ok && result.pcm_bytes > 0;

    site->Release();
    CoTaskMemFree(wfx);
    token->Release();
    with_token->Release();
    engine->Release();
    return result;
}

// %LOCALAPPDATA%\DECtalkDTC01\diagnostics-report.txt, falling back to the
// exe's own directory if LOCALAPPDATA is unavailable.
std::wstring resolve_report_path()
{
    wchar_t base[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) != 0) {
        std::wstring dir = std::wstring(base) + L"\\DECtalkDTC01";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\diagnostics-report.txt";
    }

    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) != 0) {
        std::wstring path(exe_path);
        const size_t slash = path.find_last_of(L"\\/");
        std::wstring dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
        return dir + L"\\diagnostics-report.txt";
    }
    return L"diagnostics-report.txt";
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring dll_path = resolve_dll_path(argc, argv);

    HRESULT hr_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr_init)) {
        wprintf(L"FAIL: CoInitializeEx -> 0x%08X\n", hr_init);
        return 1;
    }

    std::wstring report;
    wchar_t line[1024];

    swprintf_s(line, L"DECtalk DTC-01 SAPI5 Diagnostics\n");
    report += line;
    swprintf_s(line, L"================================\n\n");
    report += line;
    swprintf_s(line, L"DLL path used   : %s\n", dll_path.c_str());
    report += line;

    const std::wstring dir = dll_path.substr(0, dll_path.find_last_of(L"\\/") + 1);
    for (const wchar_t* lib : {L"DECtalk.dll", L"DECtalk20.dll"}) {
        const bool there = GetFileAttributesW((dir + lib).c_str()) != INVALID_FILE_ATTRIBUTES;
        swprintf_s(line, L"%-16s: %s\n", lib, there ? L"beside the DLL" : L"MISSING (its voices cannot speak)");
        report += line;
    }

    swprintf_s(line, L"Diag build      : %s\n\n", kBitness);
    report += line;

    HMODULE mod = LoadLibraryW(dll_path.c_str());
    if (!mod) {
        swprintf_s(line, L"FAIL: LoadLibrary(%s) -> %lu\n", dll_path.c_str(), GetLastError());
        report += line;
        wprintf(L"%s", report.c_str());
        CoUninitialize();
        return 1;
    }

    auto get_class = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(mod, "DllGetClassObject"));
    if (!get_class) {
        report += L"FAIL: DllGetClassObject not exported\n";
        wprintf(L"%s", report.c_str());
        FreeLibrary(mod);
        CoUninitialize();
        return 1;
    }

    int pass_count = 0;
    int fail_count = 0;

    for (int i = 0; i < dtc01::voice_count(); ++i) {
        const dtc01::VoiceDef& voice = dtc01::VOICES[i];
        VoiceResult r = run_voice(mod, get_class, voice);

        wchar_t key_w[64] = {}, fw_w[16] = {};
        const int key_chars = MultiByteToWideChar(CP_UTF8, 0, voice.key, -1, key_w, 64);
        const int fw_chars = MultiByteToWideChar(CP_UTF8, 0, voice.firmware, -1, fw_w, 16);
        if (key_chars == 0 || fw_chars == 0) {
            // voices.hpp's key/firmware strings are always short ASCII
            // literals, so this should never actually fail -- but if it
            // ever did (e.g. a future entry with unexpected bytes and a
            // buffer too small), silently proceeding with an empty key_w
            // would misreport the failing voice as some *other* voice
            // (or as blank) rather than surfacing the real problem.
            swprintf_s(line, L"[FAIL] <name conversion error for voice index %d, GetLastError=%lu>\n",
                       i, GetLastError());
            report += line;
            wprintf(L"%s", line);
            ++fail_count;
            continue;
        }

        swprintf_s(line, L"[%s] %-8s / %-4s (%s, %s) : SetToken=0x%08X Format=0x%08X(%s) Speak=0x%08X bytes=%zu dur=%.2fs\n",
                   r.pass ? L"PASS" : L"FAIL",
                   key_w, fw_w, voice.display, voice.gender,
                   r.hr_set_token, r.hr_format, r.format_ok ? L"ok" : L"BAD",
                   r.hr_speak, r.pcm_bytes, r.duration_sec);
        report += line;
        wprintf(L"%s", line);

        if (r.pass) ++pass_count; else ++fail_count;
    }

    swprintf_s(line, L"\nSummary: %d/%d voices passed\n", pass_count, pass_count + fail_count);
    report += line;
    wprintf(L"%s", line);

    FreeLibrary(mod);
    CoUninitialize();

    // Write the report to the standard location (falls back to the exe dir).
    const std::wstring report_path = resolve_report_path();
    FILE* f = _wfopen(report_path.c_str(), L"w, ccs=UTF-8");
    if (f) {
        fwrite(report.c_str(), sizeof(wchar_t), report.size(), f);
        fclose(f);
        wprintf(L"\nReport written to: %s\n", report_path.c_str());
    } else {
        wprintf(L"\nWARNING: could not write report to %s\n", report_path.c_str());
    }

    return fail_count == 0 ? 0 : 1;
}
