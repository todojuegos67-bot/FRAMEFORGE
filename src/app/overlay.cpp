#include "app/overlay.h"
#include "core/log.h"
#include <algorithm>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace ff {
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

bool Overlay::create(HINSTANCE inst, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    m_dev = dev; m_ctx = ctx;
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = OverlayProc; wc.hInstance = inst; wc.lpszClassName = L"FrameForgeOverlay";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                             wc.lpszClassName, L"FrameForge Overlay", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, inst, nullptr);
    if (!m_hwnd) return false;
    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
    if (!SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE)) logf("WDA_EXCLUDEFROMCAPTURE not available");
    std::string err;
    auto vsb = compileEmbedded("blit.hlsl", "vs", "vs_5_0", &err);
    auto psb = compileEmbedded("blit.hlsl", "ps", "ps_5_0", &err);
    if (!vsb || !psb) return false;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps);
    m_s.create(dev);
    ComPtr<IDXGIDevice> dxgi; dev->QueryInterface(IID_PPV_ARGS(&dxgi));
    ComPtr<IDXGIAdapter> ad; dxgi->GetAdapter(&ad);
    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(ad->GetParent(IID_PPV_ARGS(&f5)))) {
        BOOL t = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof t))) m_tearing = t != FALSE;
    }
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dw.GetAddressOf()));
    if (m_dw) m_dw->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"", &m_font);
    logf("Overlay created, tearing supported: %d", (int)m_tearing);
    return m_vs && m_ps;
}

bool Overlay::createSwapchain() {
    ComPtr<IDXGIDevice> dxgi; m_dev->QueryInterface(IID_PPV_ARGS(&dxgi));
    ComPtr<IDXGIAdapter> ad; dxgi->GetAdapter(&ad);
    ComPtr<IDXGIFactory2> f; ad->GetParent(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = m_w; d.Height = m_h; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    m_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    d.Flags = m_flags;
    ComPtr<IDXGISwapChain1> sc1;
    m_hr = f->CreateSwapChainForHwnd(m_dev, m_hwnd, &d, nullptr, nullptr, &sc1);
    if (FAILED(m_hr)) { logf("CreateSwapChainForHwnd failed 0x%08x", (unsigned)m_hr); return false; }
    f->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&m_sc);
    m_sc->SetMaximumFrameLatency(1);
    m_wait = m_sc->GetFrameLatencyWaitableObject();
    createTargets();
    m_ready = false;
    return true;
}

void Overlay::createTargets() {
    ComPtr<ID3D11Texture2D> bb; m_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    m_dev->CreateRenderTargetView(bb.Get(), nullptr, &m_rtv);
    if (!m_d2d) return;
    ComPtr<IDXGISurface> surf; bb.As(&surf);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (surf && SUCCEEDED(m_d2d->CreateDxgiSurfaceRenderTarget(surf.Get(), &p, &m_rt2d))) {
        m_rt2d->CreateSolidColorBrush(D2D1::ColorF(0.3f, 1.0f, 0.4f), &m_fg);
        m_rt2d->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &m_bg);
    }
}

void Overlay::drawOsd() {
    if (!m_osdOn || !m_rt2d || !m_font || m_osd.empty()) return;
    m_rt2d->BeginDraw();
    D2D1_RECT_F r = D2D1::RectF(6, 6, 6 + 9.0f * 60, 6 + 44);
    m_rt2d->FillRectangle(r, m_bg.Get());
    D2D1_RECT_F t = D2D1::RectF(12, 9, r.right, r.bottom);
    m_rt2d->DrawText(m_osd.c_str(), (UINT32)m_osd.size(), m_font.Get(), t, m_fg.Get());
    m_rt2d->EndDraw();
}

bool Overlay::place(const RECT& r) {
    UINT w = (UINT)std::max<LONG>(1, r.right - r.left), h = (UINT)std::max<LONG>(1, r.bottom - r.top);
    bool moved = !EqualRect(&r, &m_rect) || !m_visible;
    if (moved) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, r.left, r.top, (int)w, (int)h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        m_rect = r; m_visible = true;
    }
    if (!m_sc) { m_w = w; m_h = h; return createSwapchain(); }
    if (w != m_w || h != m_h) {
        m_rtv.Reset(); m_rt2d.Reset(); m_fg.Reset(); m_bg.Reset();
        m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
        m_ctx->Flush();
        m_hr = m_sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, m_flags);
        if (FAILED(m_hr)) { logf("ResizeBuffers failed 0x%08x", (unsigned)m_hr); return false; }
        m_w = w; m_h = h;
        createTargets();
        logf("Overlay resized %ux%u", w, h);
    }
    return true;
}

void Overlay::hide() {
    if (m_visible) ShowWindow(m_hwnd, SW_HIDE);
    m_visible = false;
}

void Overlay::destroy() {
    m_rtv.Reset(); m_rt2d.Reset(); m_sc.Reset();
    if (m_hwnd) DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
}

bool Overlay::ready() {
    if (!m_sc) return false;
    if (m_ready) return true;
    if (WaitForSingleObject(m_wait, 0) == WAIT_OBJECT_0) m_ready = true;
    return m_ready;
}

bool Overlay::present(ID3D11ShaderResourceView* img, const RECT& dst, bool allowTearing) {
    if (!m_sc || !m_rtv) return true;
    float black[4] = {0, 0, 0, 1};
    m_ctx->ClearRenderTargetView(m_rtv.Get(), black);
    ID3D11RenderTargetView* rt = m_rtv.Get();
    m_ctx->OMSetRenderTargets(1, &rt, nullptr);
    D3D11_VIEWPORT vp{(float)dst.left, (float)dst.top, (float)(dst.right - dst.left), (float)(dst.bottom - dst.top), 0, 1};
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ID3D11SamplerState* ss = m_s.linear.Get();
    m_ctx->PSSetSamplers(0, 1, &ss);
    m_ctx->PSSetShaderResources(0, 1, &img);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nul = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &nul);
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    drawOsd();
    UINT flags = (allowTearing && m_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    m_hr = m_sc->Present(0, flags);
    m_ready = false;
    if (m_hr == DXGI_ERROR_DEVICE_REMOVED || m_hr == DXGI_ERROR_DEVICE_RESET) {
        logf("Present: device lost 0x%08x", (unsigned)m_hr);
        return false;
    }
    return true;
}
}
