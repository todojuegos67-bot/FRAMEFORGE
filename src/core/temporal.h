#pragma once
#include "core/gpu.h"
#include <memory>
#include <string>

namespace ff {
// Inputs for a temporal upscaler, all at source (render) resolution:
//   color  RGBA8 (gamma / sRGB-encoded, LDR)
//   mv     R16G16_FLOAT, pixels, current -> previous position (prev = cur + mv * mvScale)
//   depth  R32_FLOAT, 0 = near .. 1 = far (constant if no depth estimate)
// Output: RGBA8 UAV texture at output resolution.
// Captured frames carry no camera jitter and no real depth, so these upscalers get less
// information than when integrated in an engine.
struct TemporalInputs {
    ID3D11Texture2D* color = nullptr;
    ID3D11Texture2D* mv = nullptr;
    ID3D11Texture2D* depth = nullptr;
    UINT srcW = 0, srcH = 0;
    float frameMs = 16.6f;
    float sharpness = 0.5f;
    float mvScale = 1.0f;
    bool reset = false;
};

class TemporalUpscaler {
public:
    virtual ~TemporalUpscaler() = default;
    virtual const char* name() const = 0;
    virtual bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::wstring& dllDir, std::string* err) = 0;
    virtual bool resize(UINT srcW, UINT srcH, UINT outW, UINT outH, std::string* err) = 0;
    virtual bool run(const TemporalInputs& in, ID3D11Texture2D* out) = 0;
};

std::unique_ptr<TemporalUpscaler> createFsr3Upscaler();
std::unique_ptr<TemporalUpscaler> createXessUpscaler();
}
