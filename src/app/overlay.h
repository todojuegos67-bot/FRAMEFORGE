#pragma once
#include "core/gpu.h"
#include <dxgi1_5.h>

namespace ff {
// Borderless, topmost, click-through, capture-excluded window with a flip-model swapchain.
class Overlay {
public:
    bool create(HINSTANCE inst, ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void destroy();
    bool place(const RECT& screenRect);  // shows the window and resizes buffers
    void hide();
    bool visible() const { return m_visible; }
    HWND hwnd() const { return m_hwnd; }
    HANDLE waitable() const { return m_wait; }
    bool ready();                         // frame-latency waitable signalled (non-blocking)
    void markReady() { m_ready = true; }  // when woken by the waitable in a wait loop
    // Draws img into dst (client coords) and presents. Returns false on device errors.
    bool present(ID3D11ShaderResourceView* img, const RECT& dst, bool allowTearing);
    bool tearingSupported() const { return m_tearing; }
    UINT width() const { return m_w; }
    UINT height() const { return m_h; }
    HRESULT lastError() const { return m_hr; }
private:
    bool createSwapchain();
    HWND m_hwnd = nullptr;
    ID3D11Device* m_dev = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    ComPtr<IDXGISwapChain2> m_sc;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    Samplers m_s;
    HANDLE m_wait = nullptr;
    UINT m_w = 0, m_h = 0, m_flags = 0;
    RECT m_rect{};
    bool m_tearing = false, m_visible = false, m_ready = false;
    HRESULT m_hr = S_OK;
};
}
