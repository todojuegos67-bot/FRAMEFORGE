#include "app/capture.h"
#include "core/log.h"
#include <algorithm>
#include <atomic>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
using winrt::Windows::Foundation::Metadata::ApiInformation;

namespace ff {
struct WgcCapture::Impl {
    wdx::Direct3D11::IDirect3DDevice d3d{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker arrived;
    wgc::GraphicsCaptureItem::Closed_revoker closedRev;
    wgc::Direct3D11CaptureFrame frame{nullptr};
    winrt::Windows::Graphics::SizeInt32 size{};
    HANDLE ev = nullptr;
    std::atomic<bool> closed{false};
    bool running = false;
};

WgcCapture::WgcCapture() : p(std::make_unique<Impl>()) { p->ev = CreateEventW(nullptr, FALSE, FALSE, nullptr); }
WgcCapture::~WgcCapture() { stop(); if (p->ev) CloseHandle(p->ev); }

bool WgcCapture::supported() {
    try { return wgc::GraphicsCaptureSession::IsSupported(); } catch (...) { return false; }
}

bool WgcCapture::start(ID3D11Device* dev, HWND hwnd, std::string* err) {
    stop();
    try {
        winrt::com_ptr<IDXGIDevice> dxgi;
        winrt::check_hresult(dev->QueryInterface(IID_PPV_ARGS(dxgi.put())));
        winrt::com_ptr<::IInspectable> insp;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put()));
        p->d3d = insp.as<wdx::Direct3D11::IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
        p->item = item;
        p->size = item.Size();
        p->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(p->d3d, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, p->size);
        p->session = p->pool.CreateCaptureSession(item);
        const wchar_t* cls = L"Windows.Graphics.Capture.GraphicsCaptureSession";
        if (ApiInformation::IsPropertyPresent(cls, L"IsCursorCaptureEnabled")) p->session.IsCursorCaptureEnabled(false);
        if (ApiInformation::IsPropertyPresent(cls, L"IsBorderRequired")) {
            try {
                if (ApiInformation::IsTypePresent(L"Windows.Graphics.Capture.GraphicsCaptureAccess"))
                    wgc::GraphicsCaptureAccess::RequestAccessAsync(wgc::GraphicsCaptureAccessKind::Borderless).get();
                p->session.IsBorderRequired(false);
            } catch (...) { logf("WGC: borderless capture not allowed"); }
        }
        if (ApiInformation::IsPropertyPresent(cls, L"MinUpdateInterval")) {
            try { p->session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{10000}); }  // 1 ms
            catch (...) { logf("WGC: MinUpdateInterval not applied"); }
        }
        p->closed = false;
        HANDLE ev = p->ev;
        p->arrived = p->pool.FrameArrived(winrt::auto_revoke, [ev](auto&&, auto&&) { SetEvent(ev); });
        Impl* ip = p.get();
        p->closedRev = item.Closed(winrt::auto_revoke, [ip](auto&&, auto&&) { ip->closed = true; SetEvent(ip->ev); });
        p->session.StartCapture();
        p->running = true;
        logf("WGC started %dx%d", p->size.Width, p->size.Height);
        return true;
    } catch (winrt::hresult_error const& e) {
        if (err) *err = winrt::to_string(e.message());
        logf("WGC start failed: 0x%08x %s", (unsigned)e.code(), winrt::to_string(e.message()).c_str());
    } catch (...) {
        if (err) *err = "unknown WGC error";
    }
    stop();
    return false;
}

void WgcCapture::stop() {
    try {
        p->arrived.revoke(); p->closedRev.revoke();
        if (p->frame) { p->frame.Close(); p->frame = nullptr; }
        if (p->session) { p->session.Close(); p->session = nullptr; }
        if (p->pool) { p->pool.Close(); p->pool = nullptr; }
    } catch (...) {}
    p->item = nullptr; p->d3d = nullptr;
    if (p->running) logf("WGC stopped");
    p->running = false;
}

bool WgcCapture::running() const { return p->running; }
bool WgcCapture::closed() const { return p->closed; }
HANDLE WgcCapture::event() const { return p->ev; }

void WgcCapture::release() {
    if (p->frame) { try { p->frame.Close(); } catch (...) {} p->frame = nullptr; }
}

bool WgcCapture::acquire(CapturedFrame& out) {
    if (!p->running) return false;
    release();
    try {
        wgc::Direct3D11CaptureFrame latest{nullptr};
        while (auto f = p->pool.TryGetNextFrame()) {
            if (latest) latest.Close();
            latest = f;
        }
        if (!latest) return false;
        auto cs = latest.ContentSize();
        if (cs.Width <= 0 || cs.Height <= 0) { latest.Close(); return false; }
        if (cs.Width != p->size.Width || cs.Height != p->size.Height) {
            p->size = cs;
            p->pool.Recreate(p->d3d, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, cs);
            logf("WGC content size %dx%d", cs.Width, cs.Height);
        }
        auto access = latest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ID3D11Texture2D* tex = nullptr;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&tex)));
        D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
        tex->Release();  // the frame keeps the surface alive
        out.tex = tex;
        out.w = std::min<UINT>(d.Width, (UINT)cs.Width);
        out.h = std::min<UINT>(d.Height, (UINT)cs.Height);
        out.sysTime = latest.SystemRelativeTime().count() * 1e-7;
        p->frame = latest;
        return true;
    } catch (...) {
        logf("WGC acquire failed");
        return false;
    }
}
}
