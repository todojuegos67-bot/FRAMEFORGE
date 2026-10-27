#include "common.hlsli"
Texture2D<float4> gSrc : register(t0);
RWTexture2D<float4> gOut : register(u0);
cbuffer C : register(b0) { uint2 outSize; float2 invOut; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= outSize)) return;
    gOut[id.xy] = float4(gSrc.SampleLevel(sLinear, (id.xy + 0.5) * invOut, 0).rgb, 1.0);
}
