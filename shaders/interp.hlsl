#include "common.hlsli"
// Bidirectional, occlusion-aware warp at output resolution.
Texture2D<float4> gA : register(t0);     // previous real frame (upscaled)
Texture2D<float4> gB : register(t1);     // newest real frame (upscaled)
Texture2D<float2> gF : register(t2);     // flow A->B (base px)
Texture2D<float2> gBw : register(t3);    // flow B->A (base px)
Texture2D<float2> gOcc : register(t4);   // r: A-only, g: B-only
Texture2D<float> gHud : register(t5);    // static HUD mask (base res)
RWTexture2D<float4> gOut : register(u0);
cbuffer C : register(b0) {
    uint2 outSize; float2 invOut;
    float2 flowUvScale; float2 invBase;
    float t; uint mode; uint isReal; uint hasFlow;
};

float3 hsv2rgb(float h, float s, float v) {
    float3 k = saturate(abs(frac(h + float3(0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return v * lerp(1.0, k, s);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= outSize)) return;
    float2 uv = (id.xy + 0.5) * invOut;
    float2 fuv = uv * flowUvScale;
    float3 col;
    float hud = hasFlow ? saturate(gHud.SampleLevel(sLinear, uv, 0) * 3.0) : 0.0;
    if (isReal) {
        col = gB.SampleLevel(sPoint, uv, 0).rgb;
    } else {
        float2 f01 = gF.SampleLevel(sLinear, fuv, 0);
        float2 f10 = gBw.SampleLevel(sLinear, fuv, 0);
        float2 ft0 = -(1.0 - t) * t * f01 + t * t * f10;
        float2 ft1 = (1.0 - t) * (1.0 - t) * f01 - t * (1.0 - t) * f10;
        float2 uvA = uv + ft0 * invBase;
        float2 uvB = uv + ft1 * invBase;
        float3 cA = gA.SampleLevel(sLinear, uvA, 0).rgb;
        float3 cB = gB.SampleLevel(sLinear, uvB, 0).rgb;
        float oA = gOcc.SampleLevel(sLinear, uvA * flowUvScale, 0).r;  // A pixel hidden in B -> only A sees it
        float oB = gOcc.SampleLevel(sLinear, uvB * flowUvScale, 0).g;  // B pixel hidden in A -> only B sees it
        if (any(uvA < 0) || any(uvA > 1)) oB = 1;
        if (any(uvB < 0) || any(uvB > 1)) oA = 1;
        float wA = (1.0 - t) * (1.0 - oB);
        float wB = t * (1.0 - oA);
        if (wA + wB < 1e-3) { wA = 1.0 - t; wB = t; }
        col = (cA * wA + cB * wB) / (wA + wB);
        float3 hudCol = t < 0.5 ? gA.SampleLevel(sPoint, uv, 0).rgb : gB.SampleLevel(sPoint, uv, 0).rgb;
        col = lerp(col, hudCol, hud);
    }
    if (mode == 1 && hasFlow) {
        float2 f = gF.SampleLevel(sLinear, fuv, 0);
        float m = length(f);
        col = hsv2rgb(atan2(f.y, f.x) / 6.2831853 + 0.5, saturate(m / 16.0), 0.25 + 0.75 * saturate(m / 4.0));
    } else if (mode == 2 && hasFlow) {
        float2 o = gOcc.SampleLevel(sLinear, fuv, 0);
        col = float3(o.r, o.g, 0) + luma(col) * 0.35;
    } else if (mode == 3) {
        col = lerp(col, float3(1, 0, 1), hud * 0.6);
    }
    gOut[id.xy] = float4(saturate(col), 1.0);
}
