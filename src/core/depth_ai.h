#pragma once
#include "core/gpu.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ff {
// "Modo IA": monocular depth (Depth-Anything-V2-Small, Apache-2.0, ONNX) through ONNX Runtime with
// the DirectML execution provider (CPU fallback). Runs at reduced resolution on a worker thread,
// amortised over several real frames. Output: inverse depth normalised to 0..1 (1 = near).
class DepthEstimator {
public:
    static constexpr UINT kW = 266, kH = 154;  // multiples of 14 (ViT patch size), ~16:9
    static const wchar_t* kModelUrl;
    DepthEstimator();
    ~DepthEstimator();
    static std::wstring defaultModelPath();
    bool modelPresent(const std::wstring& path) const;
    // Downloads the model from its official Hugging Face repository in the background.
    void startDownload(const std::wstring& path);
    bool downloading() const { return m_downloading; }
    std::string downloadStatus();

    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::wstring& modelPath, std::string* err);
    bool ready() const { return m_ready; }
    bool usingGpu() const { return m_gpu; }
    void submit(ID3D11ShaderResourceView* frame, UINT every);  // call once per real frame
    bool update();                                              // uploads finished results
    bool hasDepth() const { return m_haveResult; }
    ID3D11ShaderResourceView* depthSRV() { return m_haveResult ? m_depth.srv.Get() : nullptr; }
    Tex2D& depthTex() { return m_depth; }
    float inferMs() const { return m_inferMs; }
    // Synchronous single inference on a CPU RGB8 image of kW x kH (tests).
    bool inferSync(const std::vector<uint8_t>& rgba, std::vector<float>& out, std::string* err);

private:
    void worker();
    bool runModel(const std::vector<float>& in, std::vector<float>& out);
    struct OrtState;
    std::unique_ptr<OrtState> m_ort;
    ID3D11Device* m_dev = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    ComPtr<ID3D11ComputeShader> m_down;
    DynCB m_cb; Samplers m_s;
    Tex2D m_small, m_depth;
    ComPtr<ID3D11Texture2D> m_staging[2];
    int m_stIdx = 0; bool m_stPending[2] = {false, false};
    uint64_t m_frame = 0;
    std::thread m_thread;
    std::mutex m_mx; std::condition_variable m_cv;
    std::vector<float> m_in, m_out;
    bool m_hasJob = false, m_hasOut = false, m_quit = false;
    std::atomic<bool> m_busy{false}, m_downloading{false};
    std::string m_dlStatus;
    std::thread m_dlThread;
    bool m_ready = false, m_gpu = false, m_haveResult = false;
    float m_inferMs = 0;
};
}
