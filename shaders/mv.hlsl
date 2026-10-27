#include "common.hlsli"
// Builds temporal-upscaler inputs at source resolution:
//   colour RGBA8, motion vectors (current -> previous, source pixels), depth (0 near .. 1 far).
Texture2D<float4> gSrc : register(t0);
Texture2D<float2> gBw : register(t1);      // flow cur->prev, base px
Texture2D<float> gInvDepth : register(t2); // optional AI inverse depth, 1 = near
RWTexture2D<float4> oColor : register(u0);
RWTexture2D<float2> oMv : register(u1);
RWTexture2D<float> oDepth : register(u2);
cbuffer C : register(b0) {
    uint2 size; float2 inv;
    float2 flowUvScale; float flowToPx; uint hasFlow;
    uint hasDepth; float3 pad;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    float2 uv = (id.xy + 0.5) * inv;
    oColor[id.xy] = float4(gSrc.SampleLevel(sPoint, uv, 0).rgb, 1);
    oMv[id.xy] = hasFlow ? gBw.SampleLevel(sLinear, uv * flowUvScale, 0) * flowToPx : float2(0, 0);
    oDepth[id.xy] = hasDepth ? 1.0 - saturate(gInvDepth.SampleLevel(sLinear, uv, 0)) : 0.5;
}
