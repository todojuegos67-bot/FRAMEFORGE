// FrameForge: window capture -> optical flow -> spatial upscale -> frame generation -> overlay.
// Nothing is injected into the game process.
#include <windows.h>
#include <dwmapi.h>
#include <dxgi1_5.h>
#include <timeapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "core/config.h"
#include "core/framegen.h"
#include "core/gpu.h"
#include "core/log.h"
#include "core/pacer.h"
#include "core/temporal.h"
#include "core/depth_ai.h"
#include "app/capture.h"
#include "app/overlay.h"
#include <winrt/base.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace ff;

namespace {
constexpr int kHotkeyId = 1;
constexpr int kHotkeyOsd = 2;

struct WinEntry { HWND h; std::string title; };

struct App {
    HINSTANCE inst = nullptr;
    HWND ui = nullptr;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> uiSc;
    ComPtr<ID3D11RenderTargetView> uiRtv;
    bool uiResize = false;
    UINT uiW = 0, uiH = 0;

    Settings cfg;
    std::wstring iniPath;

    Overlay overlay;
    WgcCapture cap;
    FrameGen gen;
    Pacer pacer;
    Tex2D out;
    std::unique_ptr<TemporalUpscaler> fsr3, xess;
    std::string fsr3Err, xessErr;
    bool fsr3Tried = false, xessTried = false;
    DepthEstimator depth;
    std::string depthErr;

    bool running = false;
    HWND target = nullptr;
    std::string targetTitle;
    std::vector<WinEntry> windows;
    int sel = -1;
    std::string status, lastError;
    bool passthrough = false;
    bool minimized = false;

    // geometry
    D3D11_BOX crop{};
    UINT srcW = 0, srcH = 0, outW = 0, outH = 0;
    RECT overlayRect{}, dstRect{};
    int appliedDownscale = 0, appliedUpscale = -1, appliedScale = -1;
    double lastGeomCheck = 0;

    // stats
    uint64_t frameId = 0;
    int realCount = 0, outCount = 0, genCount = 0;
    double statT0 = 0, realFps = 0, outFps = 0, genFps = 0, latencyMs = 0, procMs = 0;
    bool quit = false;
} g;

const char* tr(const char* es, const char* en) { return g.cfg.lang == 1 ? en : es; }

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string windowTitle(HWND h) {
    wchar_t b[256]; int n = GetWindowTextW(h, b, 256);
    return narrow(std::wstring(b, n));
}

BOOL CALLBACK enumProc(HWND h, LPARAM) {
    if (!IsWindowVisible(h) || h == g.ui || h == g.overlay.hwnd()) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return TRUE;
    std::string t = windowTitle(h);
    if (t.empty()) return TRUE;
    g.windows.push_back({h, t});
    return TRUE;
}

void refreshWindows() {
    HWND prev = (g.sel >= 0 && g.sel < (int)g.windows.size()) ? g.windows[g.sel].h : nullptr;
    g.windows.clear(); g.sel = -1;
    EnumWindows(enumProc, 0);
    for (int i = 0; i < (int)g.windows.size(); ++i) if (g.windows[i].h == prev) g.sel = i;
}

void applyParams() {
    g.gen.params.hud = g.cfg.hudMask != 0;
    g.gen.params.hudFrames = (float)g.cfg.hudFrames;
    g.gen.params.useFsr = g.cfg.upscale == (int)UpscaleMode::Fsr1;
    g.gen.params.sharpness = g.cfg.sharpness;
    g.pacer.setMultiplier(g.passthrough ? 1 : g.cfg.multiplier);
    g.gen.mvScale = g.cfg.mvSign;
    TemporalUpscaler* t = nullptr;
    if (g.cfg.upscale == (int)UpscaleMode::Fsr3) {
        if (!g.fsr3Tried) {
            g.fsr3Tried = true;
            g.fsr3 = createFsr3Upscaler();
            if (!g.fsr3->init(g.dev.Get(), g.ctx.Get(), exeDir(), &g.fsr3Err)) g.fsr3.reset();
        }
        t = g.fsr3.get();
    } else if (g.cfg.upscale == (int)UpscaleMode::Xess) {
        if (!g.xessTried) {
            g.xessTried = true;
            g.xess = createXessUpscaler();
            std::wstring p = g.cfg.xessDll.empty() ? exeDir() : g.cfg.xessDll;
            if (!g.xess->init(g.dev.Get(), g.ctx.Get(), p, &g.xessErr)) g.xess.reset();
        }
        t = g.xess.get();
    }
    // Spatial FSR1 stays as the fallback when a temporal upscaler is missing or fails.
    if (g.cfg.upscale >= (int)UpscaleMode::Fsr3) g.gen.params.useFsr = true;
    g.gen.setTemporal(t);
}

void stopCapture(const char* why) {
    if (!g.running) return;
    g.cap.stop();
    g.overlay.hide();
    g.running = false;
    g.status = why ? why : "";
    logf("Stopped: %s", why ? why : "");
}

void startCapture(HWND h) {
    if (!h || !IsWindow(h) || h == g.ui || h == g.overlay.hwnd()) return;
    stopCapture(nullptr);
    std::string err;
    if (!g.cap.start(g.dev.Get(), h, &err)) { g.lastError = tr("No se pudo capturar: ", "Capture failed: ") + err; return; }
    g.target = h; g.targetTitle = windowTitle(h);
    g.running = true; g.lastError.clear();
    g.srcW = g.srcH = 0; g.appliedDownscale = 0; g.lastGeomCheck = 0;
    g.pacer.reset(); applyParams();
    g.status = tr("Capturando: ", "Capturing: ") + g.targetTitle;
    logf("Started capture of '%s'", g.targetTitle.c_str());
}

// Client area of the target in screen coords and its offset in the captured (frame-bounds) image.
bool targetGeometry(RECT& client, POINT& offset) {
    RECT cr; if (!GetClientRect(g.target, &cr)) return false;
    POINT tl{0, 0}; ClientToScreen(g.target, &tl);
    client = {tl.x, tl.y, tl.x + cr.right, tl.y + cr.bottom};
    RECT fb{};
    if (FAILED(DwmGetWindowAttribute(g.target, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof fb))) GetWindowRect(g.target, &fb);
    offset = {std::max<LONG>(0, client.left - fb.left), std::max<LONG>(0, client.top - fb.top)};
    return cr.right > 0 && cr.bottom > 0;
}

// Computes crop, output size and overlay placement; (re)allocates GPU resources when needed.
bool updateGeometry(UINT capW, UINT capH) {
    RECT client; POINT off;
    if (!targetGeometry(client, off)) return false;
    UINT sw = (UINT)std::min<LONG>(client.right - client.left, (LONG)capW - off.x);
    UINT sh = (UINT)std::min<LONG>(client.bottom - client.top, (LONG)capH - off.y);
    if ((int)sw <= 0 || (int)sh <= 0) return false;
    g.crop = {(UINT)off.x, (UINT)off.y, 0, (UINT)off.x + sw, (UINT)off.y + sh, 1};

    RECT ov; UINT ow = sw, oh = sh;
    if (g.cfg.upscale != (int)UpscaleMode::Off) {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(g.target, MONITOR_DEFAULTTONEAREST), &mi);
        ov = mi.rcMonitor;
        UINT mw = ov.right - ov.left, mh = ov.bottom - ov.top;
        float fit = std::min((float)mw / sw, (float)mh / sh);
        float s = g.cfg.scale == 1 ? 1.5f : g.cfg.scale == 2 ? 2.0f : g.cfg.scale == 3 ? 3.0f : fit;
        s = std::min(s, fit);
        ow = std::max(1u, (UINT)std::lround(sw * s)); oh = std::max(1u, (UINT)std::lround(sh * s));
        LONG x = (LONG)(mw - ow) / 2, y = (LONG)(mh - oh) / 2;
        g.dstRect = {x, y, x + (LONG)ow, y + (LONG)oh};
    } else {
        ov = client;
        g.dstRect = {0, 0, (LONG)sw, (LONG)sh};
    }
    if (!g.overlay.place(ov)) return false;
    g.overlayRect = ov;
    if (sw != g.srcW || sh != g.srcH || ow != g.outW || oh != g.outH || g.appliedDownscale != g.cfg.flowDownscale ||
        g.appliedUpscale != g.cfg.upscale) {
        g.appliedUpscale = g.cfg.upscale;
        applyParams();
        g.srcW = sw; g.srcH = sh; g.outW = ow; g.outH = oh; g.appliedDownscale = g.cfg.flowDownscale;
        g.gen.params.downscale = g.cfg.flowDownscale;
        bool ok = g.gen.ok() && g.gen.resize(sw, sh, ow, oh) &&
                  createTex(g.dev.Get(), ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS, g.out);
        g.passthrough = !ok;
        if (!ok) {
            logf("Falling back to pass-through");
            createTex(g.dev.Get(), sw, sh, DXGI_FORMAT_B8G8R8A8_UNORM, 0, g.out);  // used as copy target
        }
        g.pacer.reset(); applyParams();
    }
    return true;
}

void presentItem(const PaceItem& it) {
    bool okp;
    if (g.passthrough) {
        okp = g.overlay.present(g.out.srv.Get(), g.dstRect, g.cfg.vrr != 0);
    } else {
        g.gen.render(it.t, it.real, g.cfg.debugView, g.out.uav.Get());
        okp = g.overlay.present(g.out.srv.Get(), g.dstRect, g.cfg.vrr != 0);
    }
    ++g.outCount;
    if (!it.real && !g.passthrough && g.gen.hasPair()) ++g.genCount;
    if (it.real) {
        double l = (nowSeconds() - it.arrival) * 1000.0;
        g.latencyMs = g.latencyMs * 0.9 + l * 0.1;
    }
    if (!okp) {
        g.lastError = tr("Dispositivo GPU perdido; captura detenida.", "GPU device lost; capture stopped.");
        stopCapture("device lost");
    }
}

void tickCapture() {
    double now = nowSeconds();
    if (now - g.lastGeomCheck > 0.1) {
        g.lastGeomCheck = now;
        if (!IsWindow(g.target) || g.cap.closed()) { stopCapture("target closed"); return; }
        bool iconic = IsIconic(g.target) != FALSE;
        if (iconic && !g.minimized) { g.overlay.hide(); g.pacer.reset(); }
        g.minimized = iconic;
        if (!iconic && g.srcW) updateGeometry(g.crop.left + g.srcW, g.crop.top + g.srcH);
    }
    if (g.minimized) { CapturedFrame f; if (g.cap.acquire(f)) g.cap.release(); return; }

    PaceItem it;
    while (g.pacer.nextDue() <= now && g.overlay.ready() && g.pacer.popDue(now, it)) {
        presentItem(it);
        if (!g.running) return;
        now = nowSeconds();
    }
    CapturedFrame f;
    if (g.cap.acquire(f)) {
        double t0 = nowSeconds();
        if (!g.srcW || f.w < g.crop.right || f.h < g.crop.bottom || !g.overlay.visible()) updateGeometry(f.w, f.h);
        if (g.srcW) {
            if (g.passthrough) {
                D3D11_BOX b = g.crop;
                b.right = std::min(b.right, f.w); b.bottom = std::min(b.bottom, f.h);
                g.ctx->CopySubresourceRegion(g.out.tex.Get(), 0, 0, 0, 0, f.tex, 0, &b);
            } else {
                bool useDepth = g.cfg.aiDepth && g.depth.ready();
                if (useDepth) g.depth.update();
                g.gen.setDepth(useDepth ? g.depth.depthSRV() : nullptr);
                g.gen.frameMs = (float)(g.pacer.interval() * 1000.0);
                g.gen.pushFrame(f.tex, g.crop);
                if (useDepth) g.depth.submit(g.gen.sourceSRV(), (UINT)g.cfg.aiInterval);
            }
            g.pacer.onRealFrame(t0, f.sysTime, ++g.frameId);
            ++g.realCount;
        }
        g.cap.release();
        g.procMs = g.procMs * 0.9 + (nowSeconds() - t0) * 1000.0 * 0.1;
        now = nowSeconds();
        while (g.pacer.nextDue() <= now && g.overlay.ready() && g.pacer.popDue(now, it)) {
            presentItem(it);
            if (!g.running) return;
            now = nowSeconds();
        }
    }
}

// ---------------------------------------------------------------- UI window
void createUiRtv() {
    ComPtr<ID3D11Texture2D> bb;
    g.uiSc->GetBuffer(0, IID_PPV_ARGS(&bb));
    g.dev->CreateRenderTargetView(bb.Get(), nullptr, &g.uiRtv);
}

LRESULT CALLBACK UiProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    switch (m) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) { g.uiW = LOWORD(l); g.uiH = HIWORD(l); g.uiResize = true; }
        return 0;
    case WM_HOTKEY:
        if (w == kHotkeyOsd) { g.cfg.osd = !g.cfg.osd; return 0; }
        if (w == kHotkeyId) {
            if (g.running) stopCapture("hotkey");
            else startCapture(GetForegroundWindow());
        }
        return 0;
    case WM_DESTROY:
        g.quit = true; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void drawUi() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("FrameForge", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("FrameForge 0.2");
    ImGui::SameLine();
    const char* langs[] = {"Español", "English"};
    ImGui::SetNextItemWidth(110);
    ImGui::SameLine(ImGui::GetWindowWidth() - 130);
    ImGui::Combo("##lang", &g.cfg.lang, langs, 2);
    ImGui::Separator();

    if (ImGui::Button(tr("Actualizar lista", "Refresh list"))) refreshWindows();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    const char* cur = (g.sel >= 0 && g.sel < (int)g.windows.size()) ? g.windows[g.sel].title.c_str() : tr("(elige una ventana)", "(pick a window)");
    if (ImGui::BeginCombo("##win", cur)) {
        for (int i = 0; i < (int)g.windows.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(g.windows[i].title.c_str(), i == g.sel)) g.sel = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (!g.running) {
        if (ImGui::Button(tr("Iniciar", "Start"), ImVec2(120, 0)) && g.sel >= 0) startCapture(g.windows[g.sel].h);
    } else if (ImGui::Button(tr("Detener", "Stop"), ImVec2(120, 0))) stopCapture("user");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("Atajo: Ctrl+Alt+F (ventana activa)", "Hotkey: Ctrl+Alt+F (foreground window)"));
    if (!g.status.empty()) ImGui::TextWrapped("%s", g.status.c_str());
    if (!g.lastError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g.lastError.c_str());

    ImGui::SeparatorText(tr("Generación de fotogramas", "Frame generation"));
    const char* mults[] = {"x1 (off)", "x2", "x3", "x4"};
    int mi = g.cfg.multiplier - 1;
    if (ImGui::Combo(tr("Multiplicador", "Multiplier"), &mi, mults, 4)) { g.cfg.multiplier = mi + 1; applyParams(); }
    bool hud = g.cfg.hudMask != 0;
    if (ImGui::Checkbox(tr("Proteger HUD estático", "Protect static HUD"), &hud)) { g.cfg.hudMask = hud; applyParams(); }
    ImGui::SliderInt(tr("Resolución flujo (÷)", "Flow resolution (÷)"), &g.cfg.flowDownscale, 1, 4);
    const char* dbg[] = {"-", "Flujo / Flow", "Oclusión / Occlusion", "HUD", tr("Profundidad IA", "AI depth")};
    ImGui::Combo(tr("Vista depuración", "Debug view"), &g.cfg.debugView, dbg, 5);
    bool osd = g.cfg.osd != 0;
    if (ImGui::Checkbox(tr("Contador FPS en pantalla (Ctrl+Alt+H)", "On-screen FPS counter (Ctrl+Alt+H)"), &osd)) g.cfg.osd = osd;

    ImGui::SeparatorText(tr("Modo IA (opcional, experimental)", "AI mode (optional, experimental)"));
    {
        std::wstring mp = DepthEstimator::defaultModelPath();
        bool present = g.depth.modelPresent(mp);
        bool ai = g.cfg.aiDepth != 0;
        if (ImGui::Checkbox(tr("Profundidad monocular para oclusiones", "Monocular depth for occlusions"), &ai)) {
            g.cfg.aiDepth = ai;
            if (ai && present && !g.depth.ready() && !g.depth.init(g.dev.Get(), g.ctx.Get(), mp, &g.depthErr)) g.cfg.aiDepth = 0;
        }
        ImGui::TextDisabled("%s", tr("Depth-Anything-V2-Small (Apache-2.0), ONNX Runtime + DirectML", "Depth-Anything-V2-Small (Apache-2.0), ONNX Runtime + DirectML"));
        if (!present) {
            if (g.depth.downloading()) ImGui::TextUnformatted(tr("Descargando modelo...", "Downloading model..."));
            else if (ImGui::Button(tr("Descargar modelo (~100 MB, Hugging Face)", "Download model (~100 MB, Hugging Face)")))
                g.depth.startDownload(mp);
            std::string ds = g.depth.downloadStatus();
            if (!ds.empty() && !g.depth.downloading()) ImGui::Text("%s", ds.c_str());
        }
        ImGui::SliderInt(tr("Cada N fotogramas reales", "Every N real frames"), &g.cfg.aiInterval, 1, 16);
        if (g.depth.ready())
            ImGui::Text("%s: %.1f ms (%s, %s)", tr("Inferencia", "Inference"), g.depth.inferMs(), g.depth.usingGpu() ? "DirectML" : "CPU",
                        tr("hilo aparte", "worker thread"));
        if (!g.depthErr.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", g.depthErr.c_str());
        ImGui::TextWrapped("%s", tr("Coste: una inferencia cada N fotogramas en otro hilo + subida de un mapa pequeño. Calidad no medida.",
                                    "Cost: one inference every N frames on a worker thread + small upload. Quality not measured."));
    }

    ImGui::SeparatorText(tr("Escalado", "Upscaling"));
    const char* ups[] = {tr("Desactivado (1:1 sobre la ventana)", "Off (1:1 over the window)"), "FSR 1 (EASU + RCAS)",
                         tr("Bilineal", "Bilinear"), "FSR 3.1 (AMD, D3D12)", "XeSS SR (Intel)"};
    if (ImGui::Combo(tr("Escalador", "Upscaler"), &g.cfg.upscale, ups, 5)) applyParams();
    if (g.cfg.upscale == (int)UpscaleMode::Fsr3 && !g.fsr3)
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "FSR 3.1: %s -> FSR1", g.fsr3Err.c_str());
    if (g.cfg.upscale == (int)UpscaleMode::Xess && !g.xess)
        ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "XeSS: %s -> FSR1", g.xessErr.c_str());
    if (g.cfg.upscale >= (int)UpscaleMode::Fsr3) {
        if (g.gen.temporalFailed()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", tr("Escalador temporal falló; usando FSR1", "Temporal upscaler failed; using FSR1"));
        ImGui::TextWrapped("%s", tr("Aviso: la captura no tiene jitter ni profundidad real; un escalador temporal rinde menos que dentro del juego.",
                                    "Note: captured frames have no jitter or real depth; temporal upscalers do less than in-engine."));
        bool neg = g.cfg.mvSign < 0;
        if (ImGui::Checkbox(tr("Invertir vectores de movimiento", "Invert motion vectors"), &neg)) { g.cfg.mvSign = neg ? -1.0f : 1.0f; applyParams(); }
    }
    const char* scales[] = {tr("Ajustar a pantalla", "Fit to screen"), "1.5x", "2x", "3x"};
    ImGui::Combo(tr("Escala", "Scale"), &g.cfg.scale, scales, 4);
    if (ImGui::SliderFloat(tr("Nitidez", "Sharpness"), &g.cfg.sharpness, 0.0f, 1.0f, "%.2f")) applyParams();
    if (!g.gen.upscaler().fsrAvailable()) ImGui::TextDisabled("%s", tr("FSR1 no disponible (shader)", "FSR1 unavailable (shader)"));
    bool vrr = g.cfg.vrr != 0;
    if (ImGui::Checkbox(tr("Permitir tearing (VRR)", "Allow tearing (VRR)"), &vrr)) g.cfg.vrr = vrr;
    if (!g.overlay.tearingSupported()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("(no soportado)", "(unsupported)")); }

    ImGui::SeparatorText(tr("Estadísticas", "Statistics"));
    ImGui::Text("%s: %.1f", tr("FPS reales (captura)", "Real FPS (capture)"), g.realFps);
    ImGui::Text("%s: %.1f", tr("FPS generados", "Generated FPS"), g.genFps);
    ImGui::Text("%s: %.1f", tr("FPS de salida", "Output FPS"), g.outFps);
    ImGui::Text("%s: %.1f ms", tr("Latencia añadida estimada", "Estimated added latency"), g.latencyMs);
    ImGui::TextDisabled("%s", tr("(llegada del frame real -> Present, sin cola GPU/DWM)", "(real frame arrival -> Present, excl. GPU/DWM queue)"));
    ImGui::Text("%s: %.2f ms  |  CPU %s: %.2f ms", tr("Intervalo medido", "Measured interval"), g.pacer.interval() * 1000.0,
                tr("proceso", "process"), g.procMs);
    ImGui::Text("%s: %llu", tr("Generados descartados", "Generated dropped"), (unsigned long long)g.pacer.dropped());
    if (g.srcW) ImGui::Text("%ux%u -> %ux%u", g.srcW, g.srcH, g.outW, g.outH);
    ImGui::Text("%s: %s", tr("Modo", "Mode"), g.passthrough ? tr("paso directo (fallback)", "pass-through (fallback)") : tr("flujo óptico propio", "own optical flow"));

    ImGui::Separator();
    if (ImGui::Button(tr("Guardar ajustes", "Save settings"))) saveSettings(g.iniPath, g.cfg);
    ImGui::End();
}

void renderUi() {
    if (g.uiResize && g.uiW && g.uiH) {
        g.uiRtv.Reset();
        g.uiSc->ResizeBuffers(0, g.uiW, g.uiH, DXGI_FORMAT_UNKNOWN, 0);
        createUiRtv();
        g.uiResize = false;
    }
    if (IsIconic(g.ui)) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    int prevScale = g.cfg.scale, prevUp = g.cfg.upscale, prevDs = g.cfg.flowDownscale;
    drawUi();
    if (g.running && (prevScale != g.cfg.scale || prevUp != g.cfg.upscale || prevDs != g.cfg.flowDownscale) && g.srcW)
        updateGeometry(g.crop.left + g.srcW, g.crop.top + g.srcH);
    ImGui::Render();
    float c[4] = {0.08f, 0.08f, 0.1f, 1};
    ID3D11RenderTargetView* rt = g.uiRtv.Get();
    g.ctx->OMSetRenderTargets(1, &rt, nullptr);
    g.ctx->ClearRenderTargetView(rt, c);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g.uiSc->Present(0, 0);
}

bool createDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, 2, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls + 1, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx);
    if (FAILED(hr)) { logf("D3D11CreateDevice failed 0x%08x", (unsigned)hr); return false; }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(g.ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    ComPtr<IDXGIDevice> dx; g.dev.As(&dx);
    ComPtr<IDXGIAdapter> ad; dx->GetAdapter(&ad);
    DXGI_ADAPTER_DESC ad0{}; ad->GetDesc(&ad0);
    logf("GPU: %s", narrow(ad0.Description).c_str());
    return true;
}

bool createUiWindow() {
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_CLASSDC; wc.lpfnWndProc = UiProc; wc.hInstance = g.inst; wc.lpszClassName = L"FrameForgeUI";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    int w = MulDiv(460, dpi, 96), h = MulDiv(860, dpi, 96);
    g.ui = CreateWindowExW(0, wc.lpszClassName, L"FrameForge", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                           nullptr, nullptr, g.inst, nullptr);
    if (!g.ui) return false;
    RECT cr; GetClientRect(g.ui, &cr);
    g.uiW = cr.right; g.uiH = cr.bottom;
    ComPtr<IDXGIDevice> dx; g.dev.As(&dx);
    ComPtr<IDXGIAdapter> ad; dx->GetAdapter(&ad);
    ComPtr<IDXGIFactory2> f; ad->GetParent(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = g.uiW; d.Height = g.uiH; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2; d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(f->CreateSwapChainForHwnd(g.dev.Get(), g.ui, &d, nullptr, nullptr, &g.uiSc))) return false;
    createUiRtv();
    ShowWindow(g.ui, SW_SHOWDEFAULT);
    UpdateWindow(g.ui);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFontConfig fc; fc.SizePixels = 13.0f * dpi / 96.0f;
    io.Fonts->AddFontDefault(&fc);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(dpi / 96.0f);
    ImGui_ImplWin32_Init(g.ui);
    ImGui_ImplDX11_Init(g.dev.Get(), g.ctx.Get());
    return true;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g.inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    std::wstring dir = exeDir();
    logInit(dir + L"\\frameforge.log");
    g.iniPath = dir + L"\\frameforge.ini";
    if (!loadSettings(g.iniPath, g.cfg)) saveSettings(g.iniPath, g.cfg);
    logf("FrameForge 0.2 starting");

    if (!createDevice()) { MessageBoxW(nullptr, L"No se pudo crear el dispositivo Direct3D 11.", L"FrameForge", MB_ICONERROR); return 1; }
    if (!g.gen.init(g.dev.Get(), g.ctx.Get())) logf("Frame generation unavailable: %s", g.gen.error().c_str());
    if (!g.overlay.create(inst, g.dev.Get(), g.ctx.Get())) { MessageBoxW(nullptr, L"No se pudo crear el overlay.", L"FrameForge", MB_ICONERROR); return 1; }
    if (!createUiWindow()) return 1;
    if (!WgcCapture::supported()) g.lastError = tr("Windows Graphics Capture no está soportado en este sistema.", "Windows Graphics Capture is not supported on this system.");
    if (!RegisterHotKey(g.ui, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F')) logf("RegisterHotKey failed");
    if (!RegisterHotKey(g.ui, kHotkeyOsd, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'H')) logf("RegisterHotKey(H) failed");
    if (g.cfg.aiDepth && !g.depth.init(g.dev.Get(), g.ctx.Get(), DepthEstimator::defaultModelPath(), &g.depthErr)) g.cfg.aiDepth = 0;
    refreshWindows();
    applyParams();

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) { timer = CreateWaitableTimerW(nullptr, FALSE, nullptr); timeBeginPeriod(1); }
    double lastUi = 0;
    g.statT0 = nowSeconds();

    while (!g.quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g.quit = true;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (g.quit) break;
        if (g.running) tickCapture();

        double now = nowSeconds();
        if (now - g.statT0 >= 1.0) {
            g.realFps = g.realCount / (now - g.statT0); g.outFps = g.outCount / (now - g.statT0);
            g.genFps = g.genCount / (now - g.statT0);
            g.realCount = g.outCount = g.genCount = 0; g.statT0 = now;
            wchar_t osd[200];
            swprintf(osd, 200, L"FrameForge ON  x%d\nReal: %.0f fps | %s: %.0f fps | Total: %.0f fps",
                     g.passthrough ? 1 : g.cfg.multiplier, g.realFps, g.cfg.lang == 1 ? L"Generated" : L"Generados", g.genFps, g.outFps);
            g.overlay.setOsd(g.cfg.osd != 0, osd);
        }
        if (g.overlay.visible()) {
            static int lastOsd = -1;
            if (lastOsd != g.cfg.osd) { lastOsd = g.cfg.osd; g.overlay.setOsd(g.cfg.osd != 0, g.cfg.osd ? L"FrameForge ON" : L""); }
        }
        if (now - lastUi >= 1.0 / 30.0) { renderUi(); lastUi = now; }

        now = nowSeconds();
        double next = lastUi + 1.0 / 30.0;
        if (g.running) next = std::min(next, g.pacer.nextDue());
        HANDLE hs[3]; DWORD n = 0;
        int capIdx = -1, swIdx = -1;
        if (g.running) { capIdx = (int)n; hs[n++] = g.cap.event(); }
        bool due = g.running && g.pacer.nextDue() <= now + 0.0005;
        if (due && !g.overlay.ready() && g.overlay.waitable()) { swIdx = (int)n; hs[n++] = g.overlay.waitable(); }
        double wait = std::max(0.0, next - now);
        if (!due && wait > 0) {
            LARGE_INTEGER li; li.QuadPart = -(LONGLONG)(wait * 1e7);
            SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
            hs[n++] = timer;
        }
        if (n == 0 || (due && swIdx < 0)) continue;
        DWORD r = MsgWaitForMultipleObjectsEx(n, hs, due ? 5 : 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (swIdx >= 0 && r == WAIT_OBJECT_0 + (DWORD)swIdx) g.overlay.markReady();
        (void)capIdx;
    }
    stopCapture(nullptr);
    saveSettings(g.iniPath, g.cfg);
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    g.overlay.destroy();
    logf("Exit");
    return 0;
}
