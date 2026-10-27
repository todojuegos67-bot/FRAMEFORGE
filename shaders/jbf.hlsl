// Colour-guided joint bilateral smoothing of a dense flow field, weighted by match confidence.
Texture2D<float2> gF : register(t0);
Texture2D<float> gGuide : register(t1);
Texture2D<float> gCost : register(t2);
RWTexture2D<float2> gOut : register(u0);
cbuffer C : register(b0) { uint2 size; int stepPx; float sigmaL; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    float lc = gGuide[id.xy];
    float2 acc = 0; float ws = 0;
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x) {
        int2 q = clamp(int2(id.xy) + int2(x, y) * stepPx, int2(0, 0), int2(size) - 1);
        float w = exp(-abs(gGuide[q] - lc) * sigmaL) * exp(-(float)(x * x + y * y) * 0.25) / (gCost[q] * 40.0 + 0.05);
        acc += gF[q] * w; ws += w;
    }
    gOut[id.xy] = acc / ws;
}
