#pragma once
// Interpolation quality evaluation on real image sequences (Middlebury "other" set with
// ground-truth interpolated middle frames). Data is downloaded by CI for evaluation only.
#include "core/framegen.h"
#include <wincodec.h>
#include <filesystem>
#include <string>
#include <vector>

namespace eval {
using namespace ff;
struct Img { UINT w = 0, h = 0; std::vector<uint8_t> bgra; };

inline bool loadPng(const std::wstring& path, Img& img) {
    static ComPtr<IWICImagingFactory> f;
    if (!f) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)); }
    if (!f) return false;
    ComPtr<IWICBitmapDecoder> dec; ComPtr<IWICBitmapFrameDecode> fr; ComPtr<IWICFormatConverter> cv;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    dec->GetFrame(0, &fr);
    f->CreateFormatConverter(&cv);
    if (FAILED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    cv->GetSize(&img.w, &img.h);
    img.bgra.resize((size_t)img.w * img.h * 4);
    return SUCCEEDED(cv->CopyPixels(nullptr, img.w * 4, (UINT)img.bgra.size(), img.bgra.data()));
}

// out is RGBA from readback; gt is BGRA.
inline double psnr(const std::vector<uint8_t>& rgba, const Img& gt) {
    double se = 0; size_t n = 0;
    for (size_t i = 0; i < gt.bgra.size(); i += 4)
        for (int c = 0; c < 3; ++c) { double d = (double)rgba[i + c] - gt.bgra[i + 2 - c]; se += d * d; ++n; }
    double mse = se / n;
    return mse <= 1e-10 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

inline std::vector<float> lumaRGBA(const std::vector<uint8_t>& p) {
    std::vector<float> l(p.size() / 4);
    for (size_t i = 0; i < l.size(); ++i) l[i] = 0.299f * p[i * 4] + 0.587f * p[i * 4 + 1] + 0.114f * p[i * 4 + 2];
    return l;
}
inline std::vector<float> lumaBGRA(const std::vector<uint8_t>& p) {
    std::vector<float> l(p.size() / 4);
    for (size_t i = 0; i < l.size(); ++i) l[i] = 0.299f * p[i * 4 + 2] + 0.587f * p[i * 4 + 1] + 0.114f * p[i * 4];
    return l;
}

// SSIM on luma, 11x11 Gaussian (sigma 1.5), standard constants.
inline double ssim(const std::vector<float>& a, const std::vector<float>& b, UINT w, UINT h) {
    float g[11]; float gs = 0;
    for (int i = 0; i < 11; ++i) { g[i] = std::exp(-(i - 5) * (i - 5) / (2 * 1.5f * 1.5f)); gs += g[i]; }
    for (float& v : g) v /= gs;
    auto blur = [&](const std::vector<float>& s) {
        std::vector<float> t(s.size()), o(s.size());
        for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
            float acc = 0; for (int k = -5; k <= 5; ++k) { int xx = std::clamp((int)x + k, 0, (int)w - 1); acc += g[k + 5] * s[(size_t)y * w + xx]; }
            t[(size_t)y * w + x] = acc; }
        for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
            float acc = 0; for (int k = -5; k <= 5; ++k) { int yy = std::clamp((int)y + k, 0, (int)h - 1); acc += g[k + 5] * t[(size_t)yy * w + x]; }
            o[(size_t)y * w + x] = acc; }
        return o;
    };
    std::vector<float> aa(a.size()), bb(a.size()), ab(a.size());
    for (size_t i = 0; i < a.size(); ++i) { aa[i] = a[i] * a[i]; bb[i] = b[i] * b[i]; ab[i] = a[i] * b[i]; }
    auto ma = blur(a), mb = blur(b), saa = blur(aa), sbb = blur(bb), sab = blur(ab);
    const double C1 = 6.5025, C2 = 58.5225;
    double acc = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        double va = saa[i] - ma[i] * ma[i], vb = sbb[i] - mb[i] * mb[i], cov = sab[i] - ma[i] * mb[i];
        acc += ((2 * ma[i] * mb[i] + C1) * (2 * cov + C2)) / ((ma[i] * ma[i] + mb[i] * mb[i] + C1) * (va + vb + C2));
    }
    return acc / a.size();
}
}
