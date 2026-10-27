#pragma once
#include <string>
namespace ff {
enum class UpscaleMode : int { Off = 0, Fsr1 = 1, Bilinear = 2, Fsr3 = 3, Xess = 4 };
enum class ScaleMode : int { Fit = 0, X1_5 = 1, X2 = 2, X3 = 3 };
struct Settings {
    int multiplier = 2;          // 1..4 (1 = pass-through)
    int upscale = 1;             // UpscaleMode
    int scale = 0;               // ScaleMode
    float sharpness = 0.5f;      // 0..1 (1 = sharpest)
    int flowDownscale = 2;       // optical flow resolution divisor (1..4)
    int vrr = 1;                 // allow tearing when supported
    int debugView = 0;           // 0 none, 1 flow, 2 occlusion, 3 HUD
    int hudMask = 1;
    int hudFrames = 3;
    int lang = 0;                // 0 es, 1 en
    int osd = 1;                 // on-screen FPS counter in the output overlay
    int aiDepth = 0;             // Modo IA: monocular depth (off by default)
    int aiInterval = 4;          // run the depth model every N real frames
    float mvSign = 1.0f;         // motion-vector sign for temporal upscalers (+1 / -1)
    std::wstring xessDll;        // optional user-provided libxess_dx11.dll path
};
bool loadSettings(const std::wstring& path, Settings& s);
bool saveSettings(const std::wstring& path, const Settings& s);
}
