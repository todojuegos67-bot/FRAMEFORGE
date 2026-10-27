#include "common.hlsli"
// Per-pixel flow refinement at base resolution. Candidates come from the 3x3 neighbouring cells of
// the block-matching grid (plus the bilinear blend); matching cost is an edge-aware (colour-weighted)
// 5x5 SAD so pixels near a motion boundary pick the vector of the side they belong to.
Texture2D<float> gA : register(t0);
Texture2D<float> gB : register(t1);
Texture2D<float2> gGrid : register(t2);
RWTexture2D<float2> gOut : register(u0);
RWTexture2D<float> gCost : register(u1);
cbuffer C : register(b0) { uint2 size; float2 inv; uint2 gridSize; float cell; float pad; };

static float a[25];
static float w[25];
static float wsum;
static float2 pc;

float cost(float2 v) {
    float s = 0;
    [unroll] for (int y = 0; y < 5; ++y)
    [unroll] for (int x = 0; x < 5; ++x) {
        int i = y * 5 + x;
        s += w[i] * abs(a[i] - gB.SampleLevel(sLinear, (pc + float2(x - 2, y - 2) + v) * inv, 0));
    }
    return s / wsum;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    pc = id.xy + 0.5;
    float ac = gA[id.xy];
    wsum = 0;
    [unroll] for (int y = 0; y < 5; ++y)
    [unroll] for (int x = 0; x < 5; ++x) {
        int2 q = clamp(int2(id.xy) + int2(x - 2, y - 2), int2(0, 0), int2(size) - 1);
        float v = gA[q];
        int i = y * 5 + x;
        a[i] = v;
        w[i] = exp(-abs(v - ac) * 24.0) * exp(-(float)((x - 2) * (x - 2) + (y - 2) * (y - 2)) * 0.15);
        wsum += w[i];
    }
    float2 g = pc / cell;                    // continuous grid coordinate
    float2 best = gGrid.SampleLevel(sLinear, g / float2(gridSize), 0);
    float bestC = cost(best);
    int2 gi = int2(floor(g - 0.5));
    [unroll] for (int cy = 0; cy <= 1; ++cy)
    [unroll] for (int cx = 0; cx <= 1; ++cx) {
        int2 q = clamp(gi + int2(cx, cy), int2(0, 0), int2(gridSize) - 1);
        float2 v = gGrid[q];
        float c = cost(v);
        if (c < bestC) { bestC = c; best = v; }
    }
    [unroll] for (int cy2 = -1; cy2 <= 2; cy2 += 3)
    [unroll] for (int cx2 = -1; cx2 <= 2; cx2 += 3) {
        int2 q = clamp(gi + int2(cx2, cy2), int2(0, 0), int2(gridSize) - 1);
        float2 v = gGrid[q];
        float c = cost(v);
        if (c < bestC) { bestC = c; best = v; }
    }
    float steps[2] = {0.5, 0.25};
    [unroll] for (int s = 0; s < 2; ++s) {
        float2 c0 = best; float st = steps[s];
        float2 d[4] = {float2(st, 0), float2(-st, 0), float2(0, st), float2(0, -st)};
        [unroll] for (int k = 0; k < 4; ++k) {
            float c = cost(c0 + d[k]);
            if (c < bestC) { bestC = c; best = c0 + d[k]; }
        }
    }
    gOut[id.xy] = best;
    gCost[id.xy] = bestC;
}
