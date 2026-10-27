// Intel XeSS Super Resolution through its D3D11 runtime (libxess_dx11.dll), loaded at run time.
#include "core/temporal.h"
#include "core/log.h"
#include <xess/xess.h>
#include <xess/xess_d3d11.h>
#include <cmath>

namespace ff {
namespace {
class XessUpscaler final : public TemporalUpscaler {
public:
    ~XessUpscaler() override {
        if (m_ctx && m_destroy) m_destroy(m_ctx);
        if (m_mod) FreeLibrary(m_mod);
    }
    const char* name() const override { return "XeSS SR"; }

    bool init(ID3D11Device* dev, ID3D11DeviceContext*, const std::wstring& dllPath, std::string* err) override {
        m_dev = dev;
        // dllPath may be a directory (bundled DLL) or a full path to a user-provided libxess_dx11.dll.
        std::wstring p = dllPath;
        if (p.size() < 4 || _wcsicmp(p.c_str() + p.size() - 4, L".dll") != 0) p += L"\\libxess_dx11.dll";
        m_mod = LoadLibraryW(p.c_str());
        if (!m_mod) return fail(err, "libxess_dx11.dll not found");
        m_create = (decltype(m_create))GetProcAddress(m_mod, "xessD3D11CreateContext");
        m_init = (decltype(m_init))GetProcAddress(m_mod, "xessD3D11Init");
        m_exec = (decltype(m_exec))GetProcAddress(m_mod, "xessD3D11Execute");
        m_destroy = (decltype(m_destroy))GetProcAddress(m_mod, "xessDestroyContext");
        m_velScale = (decltype(m_velScale))GetProcAddress(m_mod, "xessSetVelocityScale");
        if (!m_create || !m_init || !m_exec || !m_destroy) return fail(err, "XeSS entry points missing");
        xess_result_t r = m_create(dev, &m_ctx);
        if (r != XESS_RESULT_SUCCESS) { m_ctx = nullptr; return fail(err, "xessD3D11CreateContext failed (" + std::to_string((int)r) + "); XeSS D3D11 may require an Intel GPU"); }
        logf("XeSS: context created");
        return true;
    }

    bool resize(UINT sw, UINT sh, UINT ow, UINT oh, std::string* err) override {
        if (!m_ctx) return false;
        float ratio = std::fmax((float)ow / sw, (float)oh / sh);
        xess_quality_settings_t q = ratio >= 2.6f ? XESS_QUALITY_SETTING_ULTRA_PERFORMANCE
                                  : ratio >= 1.85f ? XESS_QUALITY_SETTING_PERFORMANCE
                                  : ratio >= 1.6f ? XESS_QUALITY_SETTING_BALANCED
                                  : ratio >= 1.4f ? XESS_QUALITY_SETTING_QUALITY
                                  : ratio >= 1.2f ? XESS_QUALITY_SETTING_ULTRA_QUALITY
                                                  : XESS_QUALITY_SETTING_AA;
        xess_d3d11_init_params_t ip{};
        ip.outputResolution = {ow, oh};
        ip.qualitySetting = q;
        ip.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR;
        xess_result_t r = m_init(m_ctx, &ip);
        if (r != XESS_RESULT_SUCCESS) return fail(err, "xessD3D11Init failed (" + std::to_string((int)r) + ")");
        m_sw = sw; m_sh = sh; m_reset = true;
        logf("XeSS init %ux%u -> %ux%u quality %d", sw, sh, ow, oh, (int)q);
        return true;
    }

    bool run(const TemporalInputs& in, ID3D11Texture2D* out) override {
        if (!m_ctx) return false;
        if (m_velScale) m_velScale(m_ctx, in.mvScale, in.mvScale);
        xess_d3d11_execute_params_t e{};
        e.pColorTexture = in.color;
        e.pVelocityTexture = in.mv;
        e.pDepthTexture = in.depth;
        e.pOutputTexture = out;
        e.exposureScale = 1.0f;
        e.resetHistory = (in.reset || m_reset) ? 1u : 0u;
        e.inputWidth = in.srcW; e.inputHeight = in.srcH;
        xess_result_t r = m_exec(m_ctx, &e);
        m_reset = false;
        if (r != XESS_RESULT_SUCCESS) { logf("XeSS execute failed %d", (int)r); return false; }
        return true;
    }

private:
    bool fail(std::string* err, const std::string& m) { if (err) *err = m; logf("XeSS: %s", m.c_str()); return false; }
    HMODULE m_mod = nullptr;
    ID3D11Device* m_dev = nullptr;
    xess_context_handle_t m_ctx = nullptr;
    decltype(&xessD3D11CreateContext) m_create = nullptr;
    decltype(&xessD3D11Init) m_init = nullptr;
    decltype(&xessD3D11Execute) m_exec = nullptr;
    decltype(&xessDestroyContext) m_destroy = nullptr;
    decltype(&xessSetVelocityScale) m_velScale = nullptr;
    UINT m_sw = 0, m_sh = 0;
    bool m_reset = true;
};
}

std::unique_ptr<TemporalUpscaler> createXessUpscaler() { return std::make_unique<XessUpscaler>(); }
}
