#include "core/depth_ai.h"
#include "core/log.h"
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <urlmon.h>
#include <bcrypt.h>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace ff {
const wchar_t* DepthEstimator::kModelUrl =
    L"https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model.onnx";

struct DepthEstimator::OrtState {
    Ort::Env env{nullptr};
    Ort::Session session{nullptr};
    std::string inName, outName;
};

// SHA-256 of onnx-community/depth-anything-v2-small onnx/model.onnx (Apache-2.0), recorded by CI.
static const char* kModelSha256 = "afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c";

static std::string sha256File(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return {};
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) BCryptHashData(h, buf.data(), (ULONG)n, 0);
        unsigned char dig[32];
        if (BCryptFinishHash(h, dig, 32, 0) == 0) {
            char t[3];
            for (unsigned char c : dig) { snprintf(t, 3, "%02x", c); hex += t; }
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    fclose(f);
    return hex;
}

std::wstring DepthEstimator::defaultModelPath() { return exeDir() + L"\\models\\depth_anything_v2_small.onnx"; }

bool DepthEstimator::modelPresent(const std::wstring& path) const {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    return a.nFileSizeLow > 10'000'000 || a.nFileSizeHigh > 0;
}

std::string DepthEstimator::downloadStatus() { std::lock_guard<std::mutex> l(m_mx); return m_dlStatus; }

void DepthEstimator::startDownload(const std::wstring& path) {
    if (m_downloading) return;
    if (m_dlThread.joinable()) m_dlThread.join();
    m_downloading = true;
    { std::lock_guard<std::mutex> l(m_mx); m_dlStatus = "downloading"; }
    m_dlThread = std::thread([this, path] {
        std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring part = path + L".part";
        HRESULT hr = URLDownloadToFileW(nullptr, kModelUrl, part.c_str(), 0, nullptr);
        std::string st;
        bool hashOk = SUCCEEDED(hr) && sha256File(part) == kModelSha256;
        if (SUCCEEDED(hr) && !hashOk) logf("Depth model SHA-256 mismatch; discarded");
        if (hashOk && modelPresent(part) && MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) st = "ok";
        else { DeleteFileW(part.c_str()); st = "failed (0x" + std::to_string((unsigned)hr) + ")"; }
        logf("Depth model download: %s", st.c_str());
        { std::lock_guard<std::mutex> l(m_mx); m_dlStatus = st; }
        m_downloading = false;
    });
}

DepthEstimator::~DepthEstimator() {
    { std::lock_guard<std::mutex> l(m_mx); m_quit = true; }
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_dlThread.joinable()) m_dlThread.join();
}

bool DepthEstimator::init(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::wstring& modelPath, std::string* err) {
    if (m_ready) return true;
    m_dev = dev; m_ctx = ctx;
    std::wstring dir = exeDir();
    LoadLibraryW((dir + L"\\DirectML.dll").c_str());
    if (!LoadLibraryW((dir + L"\\onnxruntime.dll").c_str())) { if (err) *err = "onnxruntime.dll not found"; return false; }
    if (!modelPresent(modelPath)) { if (err) *err = "model not downloaded"; return false; }
    try {
        Ort::InitApi();
        m_ort = std::make_unique<OrtState>();
        m_ort->env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "frameforge");
        Ort::SessionOptions so;
        so.DisableMemPattern();
        so.SetExecutionMode(ORT_SEQUENTIAL);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        m_gpu = false;
        try {
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(so, 0));
            m_gpu = true;
        } catch (const Ort::Exception& e) { logf("DirectML EP unavailable, CPU fallback: %s", e.what()); }
        m_ort->session = Ort::Session(m_ort->env, modelPath.c_str(), so);
        Ort::AllocatorWithDefaultOptions a;
        m_ort->inName = m_ort->session.GetInputNameAllocated(0, a).get();
        m_ort->outName = m_ort->session.GetOutputNameAllocated(0, a).get();
        logf("Depth model loaded (%s), in=%s out=%s", m_gpu ? "DirectML" : "CPU", m_ort->inName.c_str(), m_ort->outName.c_str());
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        logf("Depth model init failed: %s", e.what());
        m_ort.reset();
        return false;
    }
    if (dev) {
        std::string e;
        m_down = createCS(dev, "bilinear.hlsl", "main", &e);
        if (!m_down || !m_cb.create(dev, 64) || !m_s.create(dev) ||
            !createTex(dev, kW, kH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS, m_small) ||
            !createTex(dev, kW, kH, DXGI_FORMAT_R32_FLOAT, 0, m_depth)) { if (err) *err = "GPU resources"; return false; }
        D3D11_TEXTURE2D_DESC d{}; m_small.tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& s : m_staging) dev->CreateTexture2D(&d, nullptr, &s);
        m_thread = std::thread([this] { worker(); });
    }
    m_ready = true;
    return true;
}

static void toTensor(const uint8_t* rgba, size_t pitch, std::vector<float>& t) {
    const float mean[3] = {0.485f, 0.456f, 0.406f}, stdv[3] = {0.229f, 0.224f, 0.225f};
    const size_t n = (size_t)DepthEstimator::kW * DepthEstimator::kH;
    t.resize(3 * n);
    for (UINT y = 0; y < DepthEstimator::kH; ++y)
        for (UINT x = 0; x < DepthEstimator::kW; ++x) {
            const uint8_t* p = rgba + y * pitch + x * 4;
            size_t i = (size_t)y * DepthEstimator::kW + x;
            for (int c = 0; c < 3; ++c) t[c * n + i] = (p[c] / 255.0f - mean[c]) / stdv[c];
        }
}

bool DepthEstimator::runModel(const std::vector<float>& in, std::vector<float>& out) {
    try {
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        int64_t shape[4] = {1, 3, kH, kW};
        Ort::Value inT = Ort::Value::CreateTensor<float>(mem, const_cast<float*>(in.data()), in.size(), shape, 4);
        const char* inN[] = {m_ort->inName.c_str()};
        const char* outN[] = {m_ort->outName.c_str()};
        auto t0 = std::chrono::steady_clock::now();
        auto res = m_ort->session.Run(Ort::RunOptions{nullptr}, inN, &inT, 1, outN, 1);
        m_inferMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const float* d = res[0].GetTensorData<float>();
        size_t n = res[0].GetTensorTypeAndShapeInfo().GetElementCount();
        if (n != (size_t)kW * kH) { logf("Depth output size %zu unexpected", n); return false; }
        auto [mn, mx] = std::minmax_element(d, d + n);
        float lo = *mn, sc = (*mx - *mn) > 1e-6f ? 1.0f / (*mx - *mn) : 0.0f;
        out.resize(n);
        for (size_t i = 0; i < n; ++i) out[i] = (d[i] - lo) * sc;  // relative inverse depth -> 0..1, 1 = near
        return true;
    } catch (const std::exception& e) {
        logf("Depth inference failed: %s", e.what());
        return false;
    }
}

bool DepthEstimator::inferSync(const std::vector<uint8_t>& rgba, std::vector<float>& out, std::string* err) {
    if (!m_ort) { if (err) *err = "not initialised"; return false; }
    std::vector<float> t;
    toTensor(rgba.data(), (size_t)kW * 4, t);
    bool ok = runModel(t, out);
    if (!ok && err) *err = "inference failed";
    return ok;
}

void DepthEstimator::worker() {
    std::vector<float> in, out;
    for (;;) {
        {
            std::unique_lock<std::mutex> l(m_mx);
            m_cv.wait(l, [this] { return m_quit || m_hasJob; });
            if (m_quit) return;
            in.swap(m_in); m_hasJob = false;
        }
        bool ok = runModel(in, out);
        std::lock_guard<std::mutex> l(m_mx);
        if (ok) { m_out.swap(out); m_hasOut = true; }
        m_busy = false;
    }
}

void DepthEstimator::submit(ID3D11ShaderResourceView* frame, UINT every) {
    if (!m_ready || !m_dev) return;
    if (++m_frame % std::max(1u, every) != 0) return;
    int i = m_stIdx;
    if (m_stPending[i]) return;  // previous readback in this slot not consumed yet
    struct { uint32_t size[2]; float inv[2]; } cb{{kW, kH}, {1.0f / kW, 1.0f / kH}};
    m_cb.set(m_ctx, &cb, sizeof cb);
    dispatch(m_ctx, m_down.Get(), m_cb.buf.Get(), m_s, {frame}, {m_small.uav.Get()}, divUp(kW, 8), divUp(kH, 8));
    m_ctx->CopyResource(m_staging[i].Get(), m_small.tex.Get());
    m_stPending[i] = true;
    m_stIdx ^= 1;
}

bool DepthEstimator::update() {
    if (!m_ready || !m_dev) return false;
    // Hand a finished readback to the worker (never blocks the render thread).
    for (int k = 0; k < 2; ++k) {
        int i = (m_stIdx + k) & 1;
        if (!m_stPending[i] || m_busy) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (m_ctx->Map(m_staging[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) continue;
        std::vector<float> t;
        toTensor((const uint8_t*)m.pData, m.RowPitch, t);
        m_ctx->Unmap(m_staging[i].Get(), 0);
        m_stPending[i] = false;
        {
            std::lock_guard<std::mutex> l(m_mx);
            m_in.swap(t); m_hasJob = true; m_busy = true;
        }
        m_cv.notify_one();
        break;
    }
    std::vector<float> res;
    {
        std::lock_guard<std::mutex> l(m_mx);
        if (!m_hasOut) return false;
        res.swap(m_out); m_hasOut = false;
    }
    m_ctx->UpdateSubresource(m_depth.tex.Get(), 0, nullptr, res.data(), kW * 4, 0);
    m_haveResult = true;
    return true;
}
}
