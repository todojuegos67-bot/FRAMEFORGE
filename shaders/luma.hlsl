#include "common.hlsli"
Texture2D<float4> gSrc : register(t0);
RWTexture2D<float> gOut : register(u0);
cbuffer C : register(b0) { uint2 outSize; float2 invOut; };

// Colour frame -> luma at base resolution (bilinear tap at the destination pixel centre = box filter for /2).
[numthreads(8, 8, 1)]
void mainColor(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= outSize)) return;
    float2 uv = (id.xy + 0.5) * invOut;
    gOut[id.xy] = luma(saturate(gSrc.SampleLevel(sLinear, uv, 0).rgb));
}

// Pyramid level L -> L+1 (2x2 box).
[numthreads(8, 8, 1)]
void mainLuma(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= outSize)) return;
    float2 uv = (id.xy + 0.5) * invOut;
    gOut[id.xy] = gSrc.SampleLevel(sLinear, uv, 0).r;
}
