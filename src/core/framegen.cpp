#include "core/framegen.h"
#include "core/log.h"
#include <algorithm>

namespace ff {
namespace {
struct LumaCB { uint32_t size[2]; float inv[2]; };
struct MatchCB {
    float levelSize[2]; float invLevelSize[2];
    uint32_t gridSize[2]; float invCoarseGrid[2];
    uint32_t coarsest; int32_t radius; float lambda; float cellSize;
};
struct MedianCB { uint32_t size[2]; float pad[2]; };
struct ConsistCB { uint32_t gridSize[2]; float invGridPx[2]; float cell; float thr; float pad[2]; };
struct HudCB { uint32_t size[2]; float diffThr; float edgeThr; float frames; uint32_t enabled; float pad[2]; };
struct InterpCB {
    uint32_t outSize[2]; float invOut[2];
    float flowUvScale[2]; float invBase[2];
    float t; uint32_t mode; uint32_t isReal; uint32_t hasFlow;
    uint32_t useDepth; float depthGain; uint32_t quality; float pad;
    float fbLo; float fbRamp; float pad2[2];
};
struct RefineCB { uint32_t size[2]; float inv[2]; uint32_t grid[2]; float cell; float pad; };
struct JbfCB { uint32_t size[2]; int32_t step; float sigmaL; };
struct MvCB { uint32_t size[2]; float inv[2]; float flowUvScale[2]; float flowToPx; uint32_t hasFlow; uint32_t hasDepth; float pad[3]; };
}

bool FrameGen::init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    m_dev = dev; m_ctx = ctx; m_ok = false;
    if (!m_cb.create(dev, 256) || !m_s.create(dev)) { m_err = "cb/sampler"; return false; }
    m_lumaColor = createCS(dev, "luma.hlsl", "mainColor", &m_err);
    m_lumaDown = createCS(dev, "luma.hlsl", "mainLuma", &m_err);
    m_match = createCS(dev, "match.hlsl", "main", &m_err);
    m_median = createCS(dev, "median.hlsl", "main", &m_err);
    m_consist = createCS(dev, "consist.hlsl", "main", &m_err);
    m_hudCS = createCS(dev, "hud.hlsl", "main", &m_err);
    m_interp = createCS(dev, "interp.hlsl", "main", &m_err);
    m_mvCS = createCS(dev, "mv.hlsl", "main", &m_err);
    m_refine = createCS(dev, "refine.hlsl", "main", &m_err);
    m_jbf = createCS(dev, "jbf.hlsl", "main", &m_err);
    { float one = 0.5f; createTex(dev, 1, 1, DXGI_FORMAT_R32_FLOAT, 0, m_depthDummy, &one, 4); }
    if (!m_up.init(dev, ctx)) { m_err = m_up.error; return false; }
    m_ok = m_lumaColor && m_lumaDown && m_match && m_median && m_consist && m_hudCS && m_interp && m_mvCS && m_refine && m_jbf;
    if (!m_ok) logf("FrameGen init failed: %s", m_err.c_str());
    return m_ok;
}

bool FrameGen::resize(UINT sw, UINT sh, UINT ow, UINT oh) {
    if (!m_ok) return false;
    srcW = sw; srcH = sh; outW = ow; outH = oh; m_count = 0;
    int ds = std::clamp(params.downscale, 1, 4);
    baseW = std::max(1u, divUp(sw, ds)); baseH = std::max(1u, divUp(sh, ds));
    levels = 1;
    { UINT w = baseW, h = baseH; while (levels < 7 && std::min(w, h) / 2 >= 24) { w = divUp(w, 2); h = divUp(h, 2); ++levels; } }
    const UINT U = D3D11_BIND_UNORDERED_ACCESS;
    bool r = createTex(m_dev, sw, sh, DXGI_FORMAT_B8G8R8A8_UNORM, 0, m_src);
    for (int i = 0; i < 2; ++i) {
        r = r && createTex(m_dev, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, U, m_color[i]);
        m_pyr[i].assign(levels, {});
        UINT w = baseW, h = baseH;
        for (int l = 0; l < levels; ++l) { r = r && createTex(m_dev, w, h, DXGI_FORMAT_R32_FLOAT, U, m_pyr[i][l]); w = divUp(w, 2); h = divUp(h, 2); }
        r = r && createTex(m_dev, baseW, baseH, DXGI_FORMAT_R32_FLOAT, U, m_stable[i]);
    }
    m_fwd.assign(levels, {}); m_bwd.assign(levels, {}); m_tmp.assign(levels, {});
    for (int l = 0; l < levels; ++l) {
        UINT gw = divUp(m_pyr[0][l].w, kCell), gh = divUp(m_pyr[0][l].h, kCell);
        r = r && createTex(m_dev, gw, gh, DXGI_FORMAT_R32G32_FLOAT, U, m_fwd[l]);
        r = r && createTex(m_dev, gw, gh, DXGI_FORMAT_R32G32_FLOAT, U, m_bwd[l]);
        r = r && createTex(m_dev, gw, gh, DXGI_FORMAT_R32G32_FLOAT, U, m_tmp[l]);
    }
    r = r && createTex(m_dev, m_fwd[0].w, m_fwd[0].h, DXGI_FORMAT_R32G32_FLOAT, U, m_occ);
    r = r && createTex(m_dev, baseW, baseH, DXGI_FORMAT_R32_FLOAT, U, m_hud);
    for (auto* t : {&m_dF, &m_dB, &m_dT1, &m_dT2, &m_occD})
        r = r && createTex(m_dev, baseW, baseH, DXGI_FORMAT_R32G32_FLOAT, U, *t);
    r = r && createTex(m_dev, baseW, baseH, DXGI_FORMAT_R32_FLOAT, U, m_costA);
    r = r && createTex(m_dev, sw, sh, DXGI_FORMAT_R8G8B8A8_UNORM, U, m_rgba);
    r = r && createTex(m_dev, sw, sh, DXGI_FORMAT_R16G16_FLOAT, U, m_mv);
    r = r && createTex(m_dev, sw, sh, DXGI_FORMAT_R32_FLOAT, U, m_depthIn);
    m_temporalReset = true; m_temporalFailed = false;
    if (r && m_temporal) {
        std::string e;
        if (!m_temporal->resize(sw, sh, ow, oh, &e)) { m_temporalFailed = true; logf("Temporal upscaler unavailable: %s", e.c_str()); }
    }
    if (r) {
        float z[4] = {0, 0, 0, 0};
        for (auto& t : m_stable) m_ctx->ClearUnorderedAccessViewFloat(t.uav.Get(), z);
        m_ctx->ClearUnorderedAccessViewFloat(m_hud.uav.Get(), z);
        m_ctx->ClearUnorderedAccessViewFloat(m_occ.uav.Get(), z);
    }
    logf("FrameGen resize src %ux%u out %ux%u base %ux%u levels %d -> %s", sw, sh, ow, oh, baseW, baseH, levels, r ? "ok" : "FAILED");
    return r;
}

void FrameGen::match(int l, int a, int b, std::vector<Tex2D>& out) {
    Tex2D& A = m_pyr[a][l]; Tex2D& B = m_pyr[b][l];
    bool coarsest = (l == levels - 1);
    MatchCB cb{};
    cb.levelSize[0] = (float)A.w; cb.levelSize[1] = (float)A.h;
    cb.invLevelSize[0] = 1.0f / A.w; cb.invLevelSize[1] = 1.0f / A.h;
    cb.gridSize[0] = m_tmp[l].w; cb.gridSize[1] = m_tmp[l].h;
    if (!coarsest) { cb.invCoarseGrid[0] = 1.0f / out[l + 1].w; cb.invCoarseGrid[1] = 1.0f / out[l + 1].h; }
    cb.coarsest = coarsest ? 1u : 0u; cb.radius = params.radius; cb.lambda = 0.002f; cb.cellSize = (float)kCell;
    m_cb.set(m_ctx, &cb, sizeof cb);
    dispatch(m_ctx, m_match.Get(), m_cb.buf.Get(), m_s,
             {A.srv.Get(), B.srv.Get(), coarsest ? nullptr : out[l + 1].srv.Get()},
             {m_tmp[l].uav.Get()}, divUp(cb.gridSize[0], 8), divUp(cb.gridSize[1], 8));
    MedianCB mc{{m_tmp[l].w, m_tmp[l].h}, {0, 0}};
    m_cb.set(m_ctx, &mc, sizeof mc);
    dispatch(m_ctx, m_median.Get(), m_cb.buf.Get(), m_s, {m_tmp[l].srv.Get()}, {out[l].uav.Get()},
             divUp(mc.size[0], 8), divUp(mc.size[1], 8));
}

bool FrameGen::pushFrame(ID3D11Texture2D* src, const D3D11_BOX& boxIn) {
    if (!m_ok || !m_src.tex) return false;
    D3D11_BOX box = boxIn;
    box.right = std::min(box.right, box.left + srcW);
    box.bottom = std::min(box.bottom, box.top + srcH);
    box.front = 0; box.back = 1;
    m_ctx->CopySubresourceRegion(m_src.tex.Get(), 0, 0, 0, 0, src, 0, &box);

    int nw = m_count == 0 ? 0 : 1 - m_newest;
    auto& pyr = m_pyr[nw];
    LumaCB lc{{pyr[0].w, pyr[0].h}, {1.0f / pyr[0].w, 1.0f / pyr[0].h}};
    m_cb.set(m_ctx, &lc, sizeof lc);
    dispatch(m_ctx, m_lumaColor.Get(), m_cb.buf.Get(), m_s, {m_src.srv.Get()}, {pyr[0].uav.Get()}, divUp(pyr[0].w, 8), divUp(pyr[0].h, 8));
    for (int l = 1; l < levels; ++l) {
        LumaCB c{{pyr[l].w, pyr[l].h}, {1.0f / pyr[l].w, 1.0f / pyr[l].h}};
        m_cb.set(m_ctx, &c, sizeof c);
        dispatch(m_ctx, m_lumaDown.Get(), m_cb.buf.Get(), m_s, {pyr[l - 1].srv.Get()}, {pyr[l].uav.Get()}, divUp(pyr[l].w, 8), divUp(pyr[l].h, 8));
    }
    m_newest = nw;
    ++m_count;
    if (m_count < 2) { upscaleNewest(nw); return true; }
    int prev = 1 - nw;
    for (int l = levels - 1; l >= 0; --l) {
        match(l, prev, nw, m_fwd);
        match(l, nw, prev, m_bwd);
    }
    ConsistCB cc{{m_fwd[0].w, m_fwd[0].h}, {1.0f / (m_fwd[0].w * kCell), 1.0f / (m_fwd[0].h * kCell)}, (float)kCell, params.occThr, {0, 0}};
    m_cb.set(m_ctx, &cc, sizeof cc);
    dispatch(m_ctx, m_consist.Get(), m_cb.buf.Get(), m_s, {m_fwd[0].srv.Get(), m_bwd[0].srv.Get()}, {m_occ.uav.Get()},
             divUp(cc.gridSize[0], 8), divUp(cc.gridSize[1], 8));
    if (!params.legacy) {
        densify(prev, nw, m_fwd[0], m_dF);
        densify(nw, prev, m_bwd[0], m_dB);
        ConsistCB dc{{baseW, baseH}, {1.0f / baseW, 1.0f / baseH}, 1.0f, params.occThr, {0, 0}};
        m_cb.set(m_ctx, &dc, sizeof dc);
        dispatch(m_ctx, m_consist.Get(), m_cb.buf.Get(), m_s, {m_dF.srv.Get(), m_dB.srv.Get()}, {m_occD.uav.Get()},
                 divUp(baseW, 8), divUp(baseH, 8));
    }
    HudCB hc{{baseW, baseH}, 2.5f / 255.0f, 0.12f, params.hudFrames, params.hud ? 1u : 0u, {0, 0}};
    m_cb.set(m_ctx, &hc, sizeof hc);
    int so = 1 - m_stableIdx;
    dispatch(m_ctx, m_hudCS.Get(), m_cb.buf.Get(), m_s,
             {m_pyr[prev][0].srv.Get(), m_pyr[nw][0].srv.Get(), m_stable[m_stableIdx].srv.Get()},
             {m_stable[so].uav.Get(), m_hud.uav.Get()}, divUp(baseW, 8), divUp(baseH, 8));
    m_stableIdx = so;
    // Pipeline order: flow -> upscale -> frame generation.
    upscaleNewest(nw);
    return true;
}

// Grid flow -> per-pixel (edge-aware candidate selection) -> 2 joint-bilateral passes.
void FrameGen::densify(int a, int b, Tex2D& grid, Tex2D& out) {
    RefineCB rc{{baseW, baseH}, {1.0f / baseW, 1.0f / baseH}, {grid.w, grid.h}, (float)kCell, 0};
    m_cb.set(m_ctx, &rc, sizeof rc);
    dispatch(m_ctx, m_refine.Get(), m_cb.buf.Get(), m_s, {m_pyr[a][0].srv.Get(), m_pyr[b][0].srv.Get(), grid.srv.Get()},
             {m_dT1.uav.Get(), m_costA.uav.Get()}, divUp(baseW, 8), divUp(baseH, 8));
    JbfCB j1{{baseW, baseH}, 1, 30.0f};
    m_cb.set(m_ctx, &j1, sizeof j1);
    dispatch(m_ctx, m_jbf.Get(), m_cb.buf.Get(), m_s, {m_dT1.srv.Get(), m_pyr[a][0].srv.Get(), m_costA.srv.Get()},
             {m_dT2.uav.Get()}, divUp(baseW, 8), divUp(baseH, 8));
    JbfCB j2{{baseW, baseH}, 2, 30.0f};
    m_cb.set(m_ctx, &j2, sizeof j2);
    dispatch(m_ctx, m_jbf.Get(), m_cb.buf.Get(), m_s, {m_dT2.srv.Get(), m_pyr[a][0].srv.Get(), m_costA.srv.Get()},
             {out.uav.Get()}, divUp(baseW, 8), divUp(baseH, 8));
}

void FrameGen::upscaleNewest(int nw) {
    if (m_temporal && !m_temporalFailed) {
        bool flow = m_count >= 2;
        MvCB mc{{srcW, srcH}, {1.0f / srcW, 1.0f / srcH},
                {params.legacy ? (float)baseW / (m_bwd[0].w * kCell) : 1.0f, params.legacy ? (float)baseH / (m_bwd[0].h * kCell) : 1.0f},
                (float)srcW / baseW, flow ? 1u : 0u, m_depthSRV ? 1u : 0u, {0, 0, 0}};
        m_cb.set(m_ctx, &mc, sizeof mc);
        dispatch(m_ctx, m_mvCS.Get(), m_cb.buf.Get(), m_s,
                 {m_src.srv.Get(), (params.legacy ? m_bwd[0] : m_dB).srv.Get(), m_depthSRV ? m_depthSRV : m_depthDummy.srv.Get()},
                 {m_rgba.uav.Get(), m_mv.uav.Get(), m_depthIn.uav.Get()}, divUp(srcW, 8), divUp(srcH, 8));
        TemporalInputs in;
        in.color = m_rgba.tex.Get(); in.mv = m_mv.tex.Get(); in.depth = m_depthIn.tex.Get();
        in.srcW = srcW; in.srcH = srcH; in.frameMs = frameMs; in.sharpness = params.sharpness;
        in.mvScale = mvScale; in.reset = m_temporalReset;
        m_temporalReset = false;
        if (m_temporal->run(in, m_color[nw].tex.Get())) return;
        m_temporalFailed = true;
        logf("Temporal upscaler failed; falling back to spatial");
    }
    m_up.run(m_src.srv.Get(), srcW, srcH, m_color[nw], params.useFsr, params.sharpness);
}

void FrameGen::render(float t, bool real, int mode, ID3D11UnorderedAccessView* out) {
    if (!m_ok || m_count == 0) return;
    bool pair = m_count >= 2;
    bool lg = params.legacy;
    InterpCB cb{{outW, outH}, {1.0f / outW, 1.0f / outH},
                {lg ? (float)baseW / (m_fwd[0].w * kCell) : 1.0f, lg ? (float)baseH / (m_fwd[0].h * kCell) : 1.0f},
                {1.0f / baseW, 1.0f / baseH}, t, (uint32_t)mode, (real || !pair) ? 1u : 0u, pair ? 1u : 0u,
                m_depthSRV ? 1u : 0u, depthGain, params.fallback ? 1u : 0u, 0, params.fbLo, params.fbRamp, {0, 0}};
    m_cb.set(m_ctx, &cb, sizeof cb);
    int prev = pair ? 1 - m_newest : m_newest;
    // Flow values are in base pixels; the shader converts using the flow texture size (grid*cell).
    dispatch(m_ctx, m_interp.Get(), m_cb.buf.Get(), m_s,
             {m_color[prev].srv.Get(), m_color[m_newest].srv.Get(), (lg ? m_fwd[0] : m_dF).srv.Get(),
              (lg ? m_bwd[0] : m_dB).srv.Get(), (lg ? m_occ : m_occD).srv.Get(), m_hud.srv.Get(), m_depthSRV ? m_depthSRV : m_depthDummy.srv.Get()},
             {out}, divUp(outW, 8), divUp(outH, 8));
}
}
