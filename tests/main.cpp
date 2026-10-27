// Headless tests on a WARP device: synthetic panning pattern + static HUD.
#include "core/framegen.h"
#include "core/pacer.h"
#include "core/upscale.h"
#include "core/log.h"
#include "core/temporal.h"
#include "core/depth_ai.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

using namespace ff;

static int g_fail = 0;
#define CHECK(c, ...) do { bool ok_ = (c); std::printf("[%s] ", ok_ ? "PASS" : "FAIL"); std::printf(__VA_ARGS__); std::printf("\n"); if (!ok_) ++g_fail; } while (0)

static float hash2(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}
static float vnoise(float x, float y, float cell) {
    x /= cell; y /= cell;
    int ix = (int)std::floor(x), iy = (int)std::floor(y);
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = hash2(ix, iy), b = hash2(ix + 1, iy), c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

static const int W = 256, H = 192;
static const float VX = 4.0f, VY = 2.0f;           // pan per frame (px)
static const int HX0 = 16, HX1 = 80, HY0 = 150, HY1 = 176;  // static HUD rect

static bool inHud(int x, int y) { return x >= HX0 && x < HX1 && y >= HY0 && y < HY1; }

// Background pans by (VX,VY)*k; HUD is static stripes.
static std::vector<uint8_t> makeFrame(float k) {
    std::vector<uint8_t> px((size_t)W * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float v;
            if (inHud(x, y)) v = ((x / 3) % 2) ? 0.95f : 0.05f;
            else {
                float sx = x - VX * k + 1000, sy = y - VY * k + 1000;
                v = 0.15f + 0.5f * vnoise(sx, sy, 9.0f) + 0.2f * vnoise(sx, sy, 3.5f);
            }
            uint8_t b = (uint8_t)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
            uint8_t* p = &px[((size_t)y * W + x) * 4];
            p[0] = b; p[1] = b; p[2] = b; p[3] = 255;
        }
    return px;
}

static bool upload(ID3D11Device* dev, const std::vector<uint8_t>& px, Tex2D& t) {
    return createTex(dev, W, H, DXGI_FORMAT_B8G8R8A8_UNORM, 0, t, px.data(), W * 4);
}

static void testPacer() {
    for (int m = 2; m <= 4; ++m) {
        Pacer p; p.setMultiplier(m);
        const int N = 60; const double T = 1.0 / 30.0;
        int real = 0, gen = 0; double lastDue = -1; bool ordered = true;
        int next = 0;
        for (double now = 0; now < N * T + 0.2; now += 0.0001) {
            if (next < N && now >= next * T) { p.onRealFrame(now, next * T, next); ++next; }
            PaceItem it;
            while (p.popDue(now, it)) { (it.real ? real : gen)++; if (it.due < lastDue) ordered = false; lastDue = it.due; }
        }
        int expected = (N - 1) * m + 1;
        CHECK(real + gen == expected && real == N && p.dropped() == 0 && ordered,
              "pacer x%d: real=%d gen=%d total=%d (expected %d = (real-1)*x+1) dropped=%llu", m, real, gen, real + gen,
              expected, (unsigned long long)p.dropped());
    }
}

#define SKIP(...) do { std::printf("[SKIP] "); std::printf(__VA_ARGS__); std::printf("\n"); } while (0)

// Foreground square moving over a static background; measures interpolation error near the square
// with and without a (synthetic) depth map.
static void testDepthOrdering(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    const int S = 40, V = 8;
    auto frame = [&](float k) {
        std::vector<uint8_t> px((size_t)W * H * 4);
        float x0 = 60 + V * k;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                bool fg = x >= x0 && x < x0 + S && y >= 70 && y < 70 + S;
                float v = fg ? 0.1f + 0.8f * vnoise(x - x0 + 500.0f, (float)y, 5.0f)
                             : 0.2f + 0.5f * vnoise((float)x, (float)y, 7.0f);
                uint8_t b = (uint8_t)std::clamp(v * 255.0f, 0.0f, 255.0f);
                uint8_t* p = &px[((size_t)y * W + x) * 4];
                p[0] = p[1] = p[2] = b; p[3] = 255;
            }
        return px;
    };
    std::vector<float> dm((size_t)W * H, 0.0f);
    for (int y = 66; y < 74 + S; ++y)
        for (int x = 60 + V * 2 - 4; x < 60 + V * 3 + S + 4; ++x) if (x < W) dm[(size_t)y * W + x] = 1.0f;
    Tex2D depth; createTex(dev, W, H, DXGI_FORMAT_R32_FLOAT, 0, depth, dm.data(), W * 4);
    double err[2] = {0, 0};
    for (int useDepth = 0; useDepth < 2; ++useDepth) {
        FrameGen g; g.params.downscale = 1; g.params.useFsr = false;
        g.init(dev, ctx); g.resize(W, H, W, H);
        g.setDepth(useDepth ? depth.srv.Get() : nullptr);
        Tex2D out; createTex(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS, out);
        D3D11_BOX box{0, 0, 0, (UINT)W, (UINT)H, 1};
        for (int k = 0; k <= 3; ++k) { Tex2D f; upload(dev, frame((float)k), f); g.pushFrame(f.tex.Get(), box); }
        g.render(0.5f, false, 0, out.uav.Get());
        std::vector<uint8_t> raw; readback(dev, ctx, out.tex.Get(), 4, raw);
        auto gt = frame(2.5f);
        double e = 0; int n = 0;
        for (int y = 60; y < 120; ++y)
            for (int x = 70; x < 150; ++x) { size_t i = ((size_t)y * W + x) * 4; e += std::fabs((float)raw[i] - gt[i]); ++n; }
        err[useDepth] = e / n;
    }
    CHECK(err[1] <= err[0] * 1.05 + 0.1, "depth-ordered blend near moving edge: MAE %.2f with depth vs %.2f without", err[1], err[0]);
}

static void testTemporal(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::unique_ptr<TemporalUpscaler> up) {
    std::string e;
    const char* nm = up->name();
    if (!up->init(dev, ctx, exeDir(), &e)) { SKIP("%s init on this device: %s", nm, e.c_str()); return; }
    FrameGen g; g.params.downscale = 1; g.params.useFsr = false;
    g.init(dev, ctx);
    g.setTemporal(up.get());
    const UINT OW = W * 3 / 2, OH = H * 3 / 2;
    g.resize(W, H, OW, OH);
    if (g.temporalFailed()) { SKIP("%s context creation failed on this device (see log)", nm); return; }
    Tex2D out; createTex(dev, OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS, out);
    D3D11_BOX box{0, 0, 0, (UINT)W, (UINT)H, 1};
    for (int k = 0; k < 4; ++k) { Tex2D f; upload(dev, makeFrame((float)k), f); g.pushFrame(f.tex.Get(), box); }
    if (g.temporalFailed()) { SKIP("%s dispatch failed on this device (see log)", nm); return; }
    g.render(1, true, 0, out.uav.Get());
    std::vector<uint8_t> raw; readback(dev, ctx, out.tex.Get(), 4, raw);
    auto ref = makeFrame(3);
    double mIn = 0, mOut = 0;
    for (size_t i = 0; i < ref.size(); i += 4) mIn += ref[i];
    for (size_t i = 0; i < raw.size(); i += 4) mOut += raw[i];
    mIn /= ref.size() / 4; mOut /= raw.size() / 4;
    CHECK(std::fabs(mIn - mOut) < mIn * 0.05 && SUCCEEDED(dev->GetDeviceRemovedReason()),
          "%s 1.5x via interop: mean in %.2f out %.2f", nm, mIn, mOut);
}

static void testDepthModel(const wchar_t* model) {
    DepthEstimator d; std::string e;
    if (!d.init(nullptr, nullptr, model, &e)) { CHECK(false, "depth model init: %s", e.c_str()); return; }
    std::vector<uint8_t> img((size_t)DepthEstimator::kW * DepthEstimator::kH * 4);
    // Synthetic scene: bright near disc over a darker ground gradient.
    for (UINT y = 0; y < DepthEstimator::kH; ++y)
        for (UINT x = 0; x < DepthEstimator::kW; ++x) {
            uint8_t* p = &img[((size_t)y * DepthEstimator::kW + x) * 4];
            float dx = x - 133.0f, dy = y - 77.0f;
            bool disc = dx * dx + dy * dy < 900;
            uint8_t v = disc ? 220 : (uint8_t)(40 + y);
            p[0] = v; p[1] = (uint8_t)(v * 0.9f); p[2] = (uint8_t)(v * 0.8f); p[3] = 255;
        }
    std::vector<float> out;
    bool ok = d.inferSync(img, out, &e);
    float mn = 1, mx = 0;
    for (float v : out) { mn = std::min(mn, v); mx = std::max(mx, v); }
    CHECK(ok && out.size() == (size_t)DepthEstimator::kW * DepthEstimator::kH && mx - mn > 0.5f,
          "depth model (%s) %ux%u: %.1f ms, range %.2f..%.2f", d.usingGpu() ? "DirectML" : "CPU",
          DepthEstimator::kW, DepthEstimator::kH, d.inferMs(), mn, mx);
}

int main(int argc, char** argv) {
    logInit(exeDir() + L"\\frameforge_tests.log");
    testPacer();

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    CHECK(SUCCEEDED(hr), "WARP device");
    if (FAILED(hr)) return 1;

    FrameGen gen;
    gen.params.downscale = 1;
    gen.params.useFsr = false;
    CHECK(gen.init(dev.Get(), ctx.Get()), "FrameGen shaders compile (%s)", gen.error().c_str());
    CHECK(gen.upscaler().fsrAvailable(), "FSR1 EASU/RCAS shaders compile");
    if (!gen.ok()) return 1;
    CHECK(gen.resize(W, H, W, H), "resize %dx%d (levels=%d grid=%ux%u)", W, H, gen.levels, gen.gridW(), gen.gridH());

    Tex2D out;
    createTex(dev.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS, out);
    D3D11_BOX box{0, 0, 0, (UINT)W, (UINT)H, 1};
    const int K = 6;
    int outputs = 0; const int M = 3;
    for (int k = 0; k < K; ++k) {
        Tex2D f; upload(dev.Get(), makeFrame((float)k), f);
        gen.pushFrame(f.tex.Get(), box);
        if (gen.hasPair()) for (int i = 1; i < M; ++i) { gen.render((float)i / M, false, 0, out.uav.Get()); ++outputs; }
        gen.render(1, true, 0, out.uav.Get()); ++outputs;
    }
    CHECK(outputs == (K - 1) * M + 1, "GPU pipeline x%d: %d real -> %d outputs", M, K, outputs);
    CHECK(SUCCEEDED(dev->GetDeviceRemovedReason()), "device alive");

    // --- flow sanity -------------------------------------------------------
    std::vector<uint8_t> raw;
    readback(dev.Get(), ctx.Get(), gen.flowFwd().tex.Get(), 8, raw);
    const float* fl2 = (const float*)raw.data();
    UINT gw = gen.gridW(), gh = gen.gridH();
    int good = 0, total = 0; double sx = 0, sy = 0;
    for (UINT y = 2; y < gh - 2; ++y)
        for (UINT x = 2; x < gw - 2; ++x) {
            int px = x * 4 + 2, py = y * 4 + 2;
            if (px > HX0 - 10 && px < HX1 + 10 && py > HY0 - 10 && py < HY1 + 10) continue;
            float fx = fl2[(y * gw + x) * 2], fy = fl2[(y * gw + x) * 2 + 1];
            ++total; sx += fx; sy += fy;
            if (std::fabs(fx - VX) < 0.5f && std::fabs(fy - VY) < 0.5f) ++good;
        }
    CHECK(total && good > total * 0.85, "forward flow: %d/%d cells within 0.5px of (%.1f,%.1f), mean (%.2f,%.2f)",
          good, total, VX, VY, sx / total, sy / total);
    readback(dev.Get(), ctx.Get(), gen.flowBwd().tex.Get(), 8, raw);
    fl2 = (const float*)raw.data();
    good = 0; total = 0;
    for (UINT y = 2; y < gh - 2; ++y)
        for (UINT x = 2; x < gw - 2; ++x) {
            int px = x * 4 + 2, py = y * 4 + 2;
            if (px > HX0 - 10 && px < HX1 + 10 && py > HY0 - 10 && py < HY1 + 10) continue;
            ++total;
            if (std::fabs(fl2[(y * gw + x) * 2] + VX) < 0.5f && std::fabs(fl2[(y * gw + x) * 2 + 1] + VY) < 0.5f) ++good;
        }
    CHECK(total && good > total * 0.85, "backward flow: %d/%d cells within 0.5px of (%.1f,%.1f)", good, total, -VX, -VY);

    // --- occlusion sanity --------------------------------------------------
    readback(dev.Get(), ctx.Get(), gen.occlusion().tex.Get(), 8, raw);
    const float* oc = (const float*)raw.data();
    int occAll = 0, occRight = 0, interior = 0, interiorOcc = 0;
    for (UINT y = 0; y < gh; ++y)
        for (UINT x = 0; x < gw; ++x) {
            float o = oc[(y * gw + x) * 2];
            if (o > 0.5f) ++occAll;
            if (x == gw - 1 && o > 0.5f) ++occRight;
            int px = x * 4 + 2, py = y * 4 + 2;
            bool nearHud = px > HX0 - 12 && px < HX1 + 12 && py > HY0 - 12 && py < HY1 + 12;
            if (x >= 3 && y >= 3 && x < gw - 3 && y < gh - 3 && !nearHud) { ++interior; if (o > 0.5f) ++interiorOcc; }
        }
    CHECK(occRight > (int)gh / 2, "occlusion: right border (content leaving frame) flagged in %d/%u cells", occRight, gh);
    CHECK(interiorOcc < interior * 0.05, "occlusion: interior false positives %d/%d", interiorOcc, interior);
    CHECK(occAll < (int)(gw * gh) / 4, "occlusion: total flagged %d/%u", occAll, gw * gh);

    // --- HUD mask sanity ---------------------------------------------------
    readback(dev.Get(), ctx.Get(), gen.hudMask().tex.Get(), 4, raw);
    const float* hm = (const float*)raw.data();
    int hudIn = 0, hudInN = 0, hudOut = 0, hudOutN = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float v = hm[y * W + x];
            if (inHud(x, y)) { ++hudInN; if (v > 0.5f) ++hudIn; }
            else if (x > 8 && y > 8 && x < W - 8 && y < H - 8 && !(x > HX0 - 4 && x < HX1 + 4 && y > HY0 - 4 && y < HY1 + 4)) { ++hudOutN; if (v > 0.5f) ++hudOut; }
        }
    CHECK(hudIn > hudInN * 0.8, "HUD mask: %d/%d HUD pixels detected", hudIn, hudInN);
    CHECK(hudOut < hudOutN * 0.02, "HUD mask: %d/%d moving pixels wrongly flagged", hudOut, hudOutN);

    // --- interpolation quality --------------------------------------------
    gen.render(0.5f, false, 0, out.uav.Get());
    readback(dev.Get(), ctx.Get(), out.tex.Get(), 4, raw);
    auto gt = makeFrame(K - 1.5f), fa = makeFrame(K - 2.0f), fb = makeFrame(K - 1.0f);
    double eI = 0, eBlend = 0, eHud = 0; int n = 0, nh = 0;
    for (int y = 12; y < H - 12; ++y)
        for (int x = 12; x < W - 12; ++x) {
            size_t i = ((size_t)y * W + x) * 4;
            float g = gt[i];
            if (inHud(x, y)) { eHud += std::fabs(raw[i] - g); ++nh; continue; }
            if (x > HX0 - 8 && x < HX1 + 8 && y > HY0 - 8 && y < HY1 + 8) continue;
            eI += std::fabs(raw[i] - g);
            eBlend += std::fabs(0.5f * (fa[i] + fb[i]) - g);
            ++n;
        }
    eI /= n; eBlend /= n; eHud /= std::max(nh, 1);
    CHECK(eI < eBlend * 0.5, "interp t=0.5 MAE %.2f vs naive blend %.2f (8-bit levels)", eI, eBlend);
    CHECK(eHud < 3.0, "HUD passthrough MAE %.2f", eHud);

    // --- FSR1 upscale ------------------------------------------------------
    if (gen.upscaler().fsrAvailable()) {
        Tex2D src, up;
        auto f = makeFrame(0);
        createTex(dev.Get(), W, H, DXGI_FORMAT_B8G8R8A8_UNORM, 0, src, f.data(), W * 4);
        createTex(dev.Get(), W * 3 / 2, H * 3 / 2, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS, up);
        gen.upscaler().run(src.srv.Get(), W, H, up, true, 0.5f);
        readback(dev.Get(), ctx.Get(), up.tex.Get(), 4, raw);
        double mIn = 0, mOut = 0;
        for (size_t i = 0; i < f.size(); i += 4) mIn += f[i];
        for (size_t i = 0; i < raw.size(); i += 4) mOut += raw[i];
        mIn /= f.size() / 4; mOut /= raw.size() / 4;
        CHECK(std::fabs(mIn - mOut) < mIn * 0.03, "FSR1 1.5x: mean in %.2f out %.2f", mIn, mOut);
    }
    testDepthOrdering(dev.Get(), ctx.Get());
    testTemporal(dev.Get(), ctx.Get(), createFsr3Upscaler());
    testTemporal(dev.Get(), ctx.Get(), createXessUpscaler());
    if (argc > 1) {
        wchar_t wp[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, wp, MAX_PATH);
        testDepthModel(wp);
    } else SKIP("depth model test (no model path given)");
    std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
