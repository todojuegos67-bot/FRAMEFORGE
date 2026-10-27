// AMD FSR 3.1 upscaler (FidelityFX SDK API, D3D12) driven from our D3D11 pipeline through
// shared textures and a shared fence.
#include "core/temporal.h"
#include "core/log.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include "api/include/ffx_api.h"
#include "api/include/ffx_api_types.h"
#include "api/include/ffx_api_loader.h"
#include "api/include/dx12/ffx_api_dx12.h"
#include "upscalers/include/ffx_upscale.h"

namespace ff {
namespace {
struct Shared {
    ComPtr<ID3D12Resource> r12;
    ComPtr<ID3D11Texture2D> t11;
};

class Fsr3Upscaler final : public TemporalUpscaler {
public:
    ~Fsr3Upscaler() override { destroyContext(); if (m_evt) CloseHandle(m_evt); if (m_mod) FreeLibrary(m_mod); }
    const char* name() const override { return "FSR 3.1"; }

    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::wstring& dllDir, std::string* err) override {
        m_dev11 = dev; m_ctx11 = ctx;
        m_mod = LoadLibraryW((dllDir + L"\\amd_fidelityfx_loader_dx12.dll").c_str());
        if (!m_mod) return fail(err, "amd_fidelityfx_loader_dx12.dll not found");
        ffxLoadFunctions(&m_fn, m_mod);
        if (!m_fn.CreateContext || !m_fn.Dispatch) return fail(err, "FFX loader entry points missing");
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&m_dev5))) || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&m_ctx4))))
            return fail(err, "D3D11.4 (shared fences) not available");
        ComPtr<IDXGIDevice> dx; dev->QueryInterface(IID_PPV_ARGS(&dx));
        ComPtr<IDXGIAdapter> ad; dx->GetAdapter(&ad);
        DXGI_ADAPTER_DESC desc{}; ad->GetDesc(&desc);
        ComPtr<IDXGIFactory4> f4; CreateDXGIFactory1(IID_PPV_ARGS(&f4));
        ComPtr<IDXGIAdapter> ad12;
        if (!f4 || FAILED(f4->EnumAdapterByLuid(desc.AdapterLuid, IID_PPV_ARGS(&ad12)))) return fail(err, "adapter LUID lookup failed");
        if (FAILED(D3D12CreateDevice(ad12.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_d12)))) return fail(err, "D3D12CreateDevice failed");
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(m_d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)))) return fail(err, "queue");
        for (auto& a : m_alloc)
            if (FAILED(m_d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return fail(err, "allocator");
        if (FAILED(m_d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[0].Get(), nullptr, IID_PPV_ARGS(&m_list)))) return fail(err, "list");
        m_list->Close();
        if (FAILED(m_d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence12)))) return fail(err, "fence");
        HANDLE h = nullptr;
        if (FAILED(m_d12->CreateSharedHandle(m_fence12.Get(), nullptr, GENERIC_ALL, nullptr, &h))) return fail(err, "fence handle");
        HRESULT hr = m_dev5->OpenSharedFence(h, IID_PPV_ARGS(&m_fence11));
        CloseHandle(h);
        if (FAILED(hr)) return fail(err, "OpenSharedFence failed");
        m_evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        logf("FSR3.1: D3D12 interop ready");
        return true;
    }

    bool resize(UINT sw, UINT sh, UINT ow, UINT oh, std::string* err) override {
        destroyContext();
        waitIdle();
        bool ok = makeShared(sw, sh, DXGI_FORMAT_R8G8B8A8_UNORM, false, m_color) &&
                  makeShared(sw, sh, DXGI_FORMAT_R16G16_FLOAT, false, m_mv) &&
                  makeShared(sw, sh, DXGI_FORMAT_R32_FLOAT, false, m_depth) &&
                  makeShared(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, true, m_out);
        if (!ok) return fail(err, "shared texture creation failed");
        ffxCreateBackendDX12Desc be{};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        be.device = m_d12.Get();
        ffxCreateContextDescUpscale cd{};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        cd.header.pNext = &be.header;
        cd.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        cd.maxRenderSize = {sw, sh};
        cd.maxUpscaleSize = {ow, oh};
        ffxReturnCode_t rc = m_fn.CreateContext(&m_ffx, &cd.header, nullptr);
        if (rc != FFX_API_RETURN_OK) { m_ffx = nullptr; return fail(err, "ffxCreateContext(upscale) failed rc=" + std::to_string(rc)); }
        m_sw = sw; m_sh = sh; m_ow = ow; m_oh = oh; m_reset = true;
        logf("FSR3.1 context %ux%u -> %ux%u", sw, sh, ow, oh);
        return true;
    }

    bool run(const TemporalInputs& in, ID3D11Texture2D* out) override {
        if (!m_ffx) return false;
        m_ctx11->CopyResource(m_color.t11.Get(), in.color);
        m_ctx11->CopyResource(m_mv.t11.Get(), in.mv);
        m_ctx11->CopyResource(m_depth.t11.Get(), in.depth);
        m_ctx4->Signal(m_fence11.Get(), ++m_value);
        m_ctx11->Flush();
        m_queue->Wait(m_fence12.Get(), m_value);
        m_slot ^= 1;
        if (m_fence12->GetCompletedValue() < m_slotValue[m_slot]) {  // allocator still in flight
            m_fence12->SetEventOnCompletion(m_slotValue[m_slot], m_evt);
            WaitForSingleObject(m_evt, 1000);
        }
        m_alloc[m_slot]->Reset();
        m_list->Reset(m_alloc[m_slot].Get(), nullptr);
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = m_list.Get();
        d.color = ffxApiGetResourceDX12(m_color.r12.Get(), FFX_API_RESOURCE_STATE_COMMON);
        d.depth = ffxApiGetResourceDX12(m_depth.r12.Get(), FFX_API_RESOURCE_STATE_COMMON);
        d.motionVectors = ffxApiGetResourceDX12(m_mv.r12.Get(), FFX_API_RESOURCE_STATE_COMMON);
        d.output = ffxApiGetResourceDX12(m_out.r12.Get(), FFX_API_RESOURCE_STATE_COMMON);
        d.jitterOffset = {0.0f, 0.0f};
        d.motionVectorScale = {in.mvScale, in.mvScale};
        d.renderSize = {m_sw, m_sh};
        d.upscaleSize = {m_ow, m_oh};
        d.enableSharpening = in.sharpness > 0.001f;
        d.sharpness = in.sharpness;
        d.frameTimeDelta = in.frameMs;
        d.preExposure = 1.0f;
        d.reset = in.reset || m_reset;
        d.cameraNear = 0.1f; d.cameraFar = 1000.0f;
        d.cameraFovAngleVertical = 1.0f;
        d.viewSpaceToMetersFactor = 1.0f;
        d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        ffxReturnCode_t rc = m_fn.Dispatch(&m_ffx, &d.header);
        m_list->Close();
        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        m_queue->Signal(m_fence12.Get(), ++m_value);
        m_slotValue[m_slot] = m_value;
        m_ctx4->Wait(m_fence11.Get(), m_value);
        m_reset = false;
        if (rc != FFX_API_RETURN_OK) { logf("FSR3.1 dispatch rc=%u", rc); return false; }
        m_ctx11->CopyResource(out, m_out.t11.Get());
        return true;
    }

private:
    bool fail(std::string* err, const std::string& m) { if (err) *err = m; logf("FSR3.1: %s", m.c_str()); return false; }

    bool makeShared(UINT w, UINT hgt, DXGI_FORMAT fmt, bool uav, Shared& s) {
        s = {};
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = hgt;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = fmt; rd.SampleDesc.Count = 1;
        rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(m_d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s.r12))))
            return false;
        HANDLE h = nullptr;
        if (FAILED(m_d12->CreateSharedHandle(s.r12.Get(), nullptr, GENERIC_ALL, nullptr, &h))) return false;
        HRESULT hr = m_dev5->OpenSharedResource1(h, IID_PPV_ARGS(&s.t11));
        CloseHandle(h);
        return SUCCEEDED(hr);
    }

    void waitIdle() {
        if (!m_queue) return;
        m_queue->Signal(m_fence12.Get(), ++m_value);
        if (m_fence12->GetCompletedValue() < m_value) {
            m_fence12->SetEventOnCompletion(m_value, m_evt);
            WaitForSingleObject(m_evt, 2000);
        }
    }

    void destroyContext() {
        if (m_ffx) { waitIdle(); m_fn.DestroyContext(&m_ffx, nullptr); m_ffx = nullptr; }
    }

    HMODULE m_mod = nullptr;
    ffxFunctions m_fn{};
    ffxContext m_ffx = nullptr;
    ID3D11Device* m_dev11 = nullptr;
    ID3D11DeviceContext* m_ctx11 = nullptr;
    ComPtr<ID3D11Device5> m_dev5;
    ComPtr<ID3D11DeviceContext4> m_ctx4;
    ComPtr<ID3D12Device> m_d12;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12CommandAllocator> m_alloc[2];
    UINT64 m_slotValue[2] = {0, 0};
    int m_slot = 0;
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12Fence> m_fence12;
    ComPtr<ID3D11Fence> m_fence11;
    UINT64 m_value = 0;
    HANDLE m_evt = nullptr;
    Shared m_color, m_mv, m_depth, m_out;
    UINT m_sw = 0, m_sh = 0, m_ow = 0, m_oh = 0;
    bool m_reset = true;
};
}

std::unique_ptr<TemporalUpscaler> createFsr3Upscaler() { return std::make_unique<Fsr3Upscaler>(); }
}
