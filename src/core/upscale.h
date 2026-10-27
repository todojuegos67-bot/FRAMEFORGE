#pragma once
#include "core/gpu.h"

namespace ff {
// Spatial upscaler: FSR 1 (EASU + RCAS) or bilinear. Output is RGBA8 (UAV) at destination size.
class Upscaler {
public:
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    bool fsrAvailable() const { return m_fsrOk; }
    // useFsr: EASU+RCAS when upscaling; otherwise bilinear (also used for 1:1 BGRA->RGBA copies).
    // sharpness 0..1 (1 = sharpest). Returns false on failure.
    bool run(ID3D11ShaderResourceView* src, UINT sw, UINT sh, Tex2D& dst, bool useFsr, float sharpness);
    std::string error;
private:
    ID3D11Device* m_dev = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    ComPtr<ID3D11ComputeShader> m_easu, m_rcas, m_bilinear;
    DynCB m_cb; Samplers m_s; Tex2D m_tmp;
    bool m_fsrOk = false;
};
float rcasStops(float sharpness01);
}
