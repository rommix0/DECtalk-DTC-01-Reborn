// DectalkTtsEngine.hpp - the SAPI5 TTS engine coclass for the DECtalk DTC-01.
//
// Implements ISpTTSEngine (Speak / GetOutputFormat) and ISpObjectWithToken
// (SetObjectToken / GetObjectToken); IUnknown is supplied by com.hpp's
// IUnknownImpl<T> template (this class is only ever instantiated through
// dectalk::com::object<DectalkTtsEngine>, exactly like BstSpeech's
// ISpTTSEngineImpl).
//
// It speaks through the speech library (DECtalk.dll, src/api; dectalk_lib.hpp), one engine per firmware and
// process, with real word boundaries from the library's index marks (REFERENCE.md s18.21).
#pragma once

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <comdef.h>

#include <mutex>
#include <string>

#include "com.hpp"
#include "voices.hpp"

namespace dtc01 {
namespace sapi {

class __declspec(uuid("{E877CE12-F153-40DF-AEAC-2AFA4D4A3DE8}")) DectalkTtsEngine :
    public ISpTTSEngine, public ISpObjectWithToken
{
public:
    DectalkTtsEngine();
    ~DectalkTtsEngine();

    DectalkTtsEngine(const DectalkTtsEngine&) = delete;
    DectalkTtsEngine& operator=(const DectalkTtsEngine&) = delete;

    // ISpTTSEngine
    STDMETHOD(Speak)(DWORD dwSpeakFlags, REFGUID rguidFormatId,
                     const WAVEFORMATEX* pWaveFormatEx, const SPVTEXTFRAG* pTextFragList,
                     ISpTTSEngineSite* pOutputSite) override;
    STDMETHOD(GetOutputFormat)(const GUID* pTargetFmtId, const WAVEFORMATEX* pTargetWaveFormatEx,
                               GUID* pOutputFormatId, WAVEFORMATEX** ppCoMemOutputWaveFormatEx) override;

    // ISpObjectWithToken
    STDMETHOD(SetObjectToken)(ISpObjectToken* pToken) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** ppToken) override;

protected:
    // Consulted by com::IUnknownImpl<DectalkTtsEngine>::QueryInterface.
    [[nodiscard]] void* get_interface(REFIID riid) noexcept
    {
        void* ptr = dectalk::com::try_primary_interface<ISpTTSEngine>(this, riid);
        return ptr ? ptr : dectalk::com::try_interface<ISpObjectWithToken>(this, riid);
    }

private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpDataKey, __uuidof(ISpDataKey));

    ISpObjectTokenPtr token_;

    // Resolved from the token's Attributes in SetObjectToken.
    std::string voice_key_;   // e.g. "paul"
    std::string firmware_;    // "v20" / "v18"
    std::string mnemonic_;    // e.g. "np"
    bool voice_resolved_ = false;

    std::mutex mu_;           // guards the members above (SetObjectToken against Speak)
};

}  // namespace sapi
}  // namespace dtc01
