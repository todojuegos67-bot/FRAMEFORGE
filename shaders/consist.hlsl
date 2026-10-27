// Forward/backward consistency -> occlusion masks.
// r: pixel of A (prev) not visible in B (cur). g: pixel of B not visible in A (disocclusion).
Texture2D<float2> gF : register(t0);
Texture2D<float2> gBw : register(t1);
RWTexture2D<float2> gOcc : register(u0);
SamplerState sLinear : register(s0);
cbuffer C : register(b0) { uint2 gridSize; float2 invGridPx; float cell; float thr; float2 pad; };

float occ(float2 pc, float2 v, Texture2D<float2> other) {
    float2 q = pc + v;
    float2 uv = q * invGridPx;
    if (any(uv < 0) || any(uv > 1)) return 1.0;
    float2 w = other.SampleLevel(sLinear, uv, 0);
    float err = length(v + w);
    float t = thr + 0.05 * (length(v) + length(w));
    return saturate((err - t) / t);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gridSize)) return;
    float2 pc = (id.xy + 0.5) * cell;
    gOcc[id.xy] = float2(occ(pc, gF[id.xy], gBw), occ(pc, gBw[id.xy], gF));
}
