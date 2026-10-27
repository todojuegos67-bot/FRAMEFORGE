#include "core/upscale.h"
#include "core/log.h"
#include <cstring>
#include <cmath>

namespace ff {
static uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static uint16_t toHalf(float f) {
    uint32_t x = fbits(f);
    uint32_t sign = (x >> 16) & 0x8000u;
    int exp = (int)((x >> 23) & 0xff) - 127 + 15;
    uint32_t man = x & 0x7fffffu;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
    return (uint16_t)(sign | (exp << 10) | (man >> 13));
}

float rcasStops(float s) { return (1.0f - s) * 2.0f; }  // FSR: 0 = max sharpness, 2 = soft

bool Upscaler::init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    m_dev = dev; m_ctx = ctx;
    if (!m_cb.create(dev, 256) || !m_s.create(dev)) { error = "cb/sampler"; return false; }
    m_bilinear = createCS(dev, "bilinear.hlsl", "main", &error);
    if (!m_bilinear) return false;
    std::string e;
    m_easu = createCS(dev, "fsr_easu.hlsl", "main", &e);
    m_rcas = createCS(dev, "fsr_rcas.hlsl", "main", &e);
    m_fsrOk = m_easu && m_rcas;
    if (!m_fsrOk) { logf("FSR1 unavailable, bilinear only: %s", e.c_str()); error = e; }
    return true;
}

bool Upscaler::run(ID3D11ShaderResourceView* src, UINT sw, UINT sh, Tex2D& dst, bool useFsr, float sharp) {
    if (!m_bilinear || !dst.uav) return false;
    UINT dw = dst.w, dh = dst.h;
    if (useFsr && m_fsrOk && (dw > sw || dh > sh)) {
        if (m_tmp.w != dw || m_tmp.h != dh)
            if (!createTex(m_dev, dw, dh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS, m_tmp)) return false;
        uint32_t c[16] = {};
        float iw = (float)sw, ih = (float)sh, ow = (float)dw, oh = (float)dh;
        c[0] = fbits(iw / ow); c[1] = fbits(ih / oh);
        c[2] = fbits(0.5f * iw / ow - 0.5f); c[3] = fbits(0.5f * ih / oh - 0.5f);
        c[4] = fbits(1.0f / iw); c[5] = fbits(1.0f / ih); c[6] = fbits(1.0f / iw); c[7] = fbits(-1.0f / ih);
        c[8] = fbits(-1.0f / iw); c[9] = fbits(2.0f / ih); c[10] = fbits(1.0f / iw); c[11] = fbits(2.0f / ih);
        c[12] = fbits(0.0f); c[13] = fbits(4.0f / ih); c[14] = 0; c[15] = 0;
        m_cb.set(m_ctx, c, sizeof c);
        dispatch(m_ctx, m_easu.Get(), m_cb.buf.Get(), m_s, {src}, {m_tmp.uav.Get()}, divUp(dw, 16), divUp(dh, 16));
        float s = std::exp2(-rcasStops(sharp));
        uint32_t r[4] = {fbits(s), (uint32_t)toHalf(s) | ((uint32_t)toHalf(s) << 16), 0, 0};
        m_cb.set(m_ctx, r, sizeof r);
        dispatch(m_ctx, m_rcas.Get(), m_cb.buf.Get(), m_s, {m_tmp.srv.Get()}, {dst.uav.Get()}, divUp(dw, 16), divUp(dh, 16));
        return true;
    }
    struct { uint32_t size[2]; float inv[2]; } cb{{dw, dh}, {1.0f / dw, 1.0f / dh}};
    m_cb.set(m_ctx, &cb, sizeof cb);
    dispatch(m_ctx, m_bilinear.Get(), m_cb.buf.Get(), m_s, {src}, {dst.uav.Get()}, divUp(dw, 8), divUp(dh, 8));
    return true;
}
}
