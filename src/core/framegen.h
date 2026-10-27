#pragma once
#include "core/gpu.h"
#include "core/upscale.h"
#include "core/temporal.h"
#include <vector>

namespace ff {
// Optical-flow frame generator.
// Pipeline per real frame: copy -> luma pyramid -> fwd/bwd hierarchical block matching (+sub-pixel)
// -> vector median -> fwd/bwd consistency (occlusion) -> static-HUD mask -> spatial upscale of the
// colour frame. Generated frames are synthesised at the OUTPUT resolution from the upscaled frames.
class FrameGen {
public:
    struct Params {
        int downscale = 2;        // flow base resolution divisor
        int radius = 4;           // full search radius at the coarsest level
        bool hud = true;
        float hudFrames = 3;
        float occThr = 1.0f;      // px
        bool useFsr = true;
        float sharpness = 0.5f;
    } params;

    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    bool ok() const { return m_ok; }
    const std::string& error() const { return m_err; }
    // Source (capture crop) size and output size.
    bool resize(UINT srcW, UINT srcH, UINT outW, UINT outH);
    // Copies the region 'box' of src (B8G8R8A8) as the new real frame and processes it.
    bool pushFrame(ID3D11Texture2D* src, const D3D11_BOX& box);
    bool hasPair() const { return m_count >= 2; }
    // Writes into out (RGBA8 UAV, output size). real => newest frame, else phase t in (0,1).
    void render(float t, bool real, int debugMode, ID3D11UnorderedAccessView* out);
    // Newest captured frame at source resolution (for pass-through fallback).
    ID3D11ShaderResourceView* sourceSRV() { return m_src.srv.Get(); }
    Upscaler& upscaler() { return m_up; }
    // Optional temporal upscaler (FSR 3.1 / XeSS). Not owned. nullptr = spatial upscaler.
    void setTemporal(TemporalUpscaler* t) { m_temporal = t; m_temporalReset = true; }
    bool temporalFailed() const { return m_temporalFailed; }
    // Optional inverse-depth map (1 = near), any resolution; nullptr disables depth use.
    void setDepth(ID3D11ShaderResourceView* d) { m_depthSRV = d; }
    float frameMs = 16.6f;
    float mvScale = 1.0f;
    float depthGain = 8.0f;

    UINT srcW = 0, srcH = 0, outW = 0, outH = 0, baseW = 0, baseH = 0;
    int levels = 0;
    static constexpr UINT kCell = 4;
    // Exposed for tests / debug.
    Tex2D& flowFwd() { return m_fwd[0]; }
    Tex2D& flowBwd() { return m_bwd[0]; }
    Tex2D& occlusion() { return m_occ; }
    Tex2D& hudMask() { return m_hud; }
    UINT gridW(int l = 0) const { return m_fwd[l].w; }
    UINT gridH(int l = 0) const { return m_fwd[l].h; }
private:
    void match(int level, int a, int b, std::vector<Tex2D>& out);
    ID3D11Device* m_dev = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    ComPtr<ID3D11ComputeShader> m_lumaColor, m_lumaDown, m_match, m_median, m_consist, m_hudCS, m_interp;
    DynCB m_cb; Samplers m_s;
    Upscaler m_up;
    Tex2D m_src;                       // newest frame, source res, BGRA8
    Tex2D m_color[2];                  // upscaled frames, output res, RGBA8
    std::vector<Tex2D> m_pyr[2];       // luma pyramids
    std::vector<Tex2D> m_fwd, m_bwd, m_tmp;
    Tex2D m_occ, m_stable[2], m_hud;
    Tex2D m_rgba, m_mv, m_depthIn;     // temporal-upscaler inputs (source res)
    ComPtr<ID3D11ComputeShader> m_mvCS;
    TemporalUpscaler* m_temporal = nullptr;
    bool m_temporalReset = true, m_temporalFailed = false;
    ID3D11ShaderResourceView* m_depthSRV = nullptr;
    Tex2D m_depthDummy;
    void upscaleNewest(int nw);
    int m_newest = 0, m_stableIdx = 0;
    uint64_t m_count = 0;
    bool m_ok = false;
    std::string m_err;
};
}
