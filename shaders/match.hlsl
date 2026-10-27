#include "common.hlsli"
// Hierarchical block matching for one pyramid level. One vector per 4x4 cell, cost = mean absolute
// difference over an 8x8 window (bilinear taps in B for sub-pixel), plus a small smoothness penalty
// towards the prediction from the coarser level.
Texture2D<float>  gA : register(t0);
Texture2D<float>  gB : register(t1);
Texture2D<float2> gCoarse : register(t2);
RWTexture2D<float2> gOut : register(u0);
cbuffer C : register(b0) {
    float2 levelSize; float2 invLevelSize;
    uint2 gridSize; float2 invCoarseGrid;
    uint coarsest; int radius; float lambda; float cellSize;
};

static float a[64];
static float2 pc;
static float2 pred;

float cost(float2 v) {
    float s = 0;
    [unroll] for (int y = 0; y < 8; ++y)
    [unroll] for (int x = 0; x < 8; ++x) {
        float2 p = pc + float2(x - 3.5, y - 3.5) + v;
        s += abs(a[y * 8 + x] - gB.SampleLevel(sLinear, p * invLevelSize, 0));
    }
    return s * (1.0 / 64.0) + lambda * length(v - pred);
}

void tryV(float2 v, inout float2 best, inout float bestC) {
    float c = cost(v);
    if (c < bestC) { bestC = c; best = v; }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= gridSize)) return;
    pc = (id.xy + 0.5) * cellSize;
    [unroll] for (int y = 0; y < 8; ++y)
    [unroll] for (int x = 0; x < 8; ++x)
        a[y * 8 + x] = gA.SampleLevel(sLinear, (pc + float2(x - 3.5, y - 3.5)) * invLevelSize, 0);

    float2 best = 0;
    pred = 0;
    float bestC;
    if (coarsest) {
        bestC = cost(0);
        [loop] for (int sy = -radius; sy <= radius; ++sy)
        [loop] for (int sx = -radius; sx <= radius; ++sx)
            tryV(float2(sx, sy), best, bestC);
    } else {
        float2 g = (pc * 0.5) / cellSize;          // continuous coordinate in the coarse grid
        float2 uv = g * invCoarseGrid;
        pred = gCoarse.SampleLevel(sLinear, uv, 0) * 2.0;
        best = pred; bestC = cost(pred);
        tryV(0, best, bestC);
        tryV(gCoarse.SampleLevel(sPoint, uv + float2( invCoarseGrid.x, 0), 0) * 2.0, best, bestC);
        tryV(gCoarse.SampleLevel(sPoint, uv + float2(-invCoarseGrid.x, 0), 0) * 2.0, best, bestC);
        tryV(gCoarse.SampleLevel(sPoint, uv + float2(0,  invCoarseGrid.y), 0) * 2.0, best, bestC);
        tryV(gCoarse.SampleLevel(sPoint, uv + float2(0, -invCoarseGrid.y), 0) * 2.0, best, bestC);
    }
    // Refinement: integer (twice), half and quarter pixel.
    float steps[4] = {1.0, 1.0, 0.5, 0.25};
    [loop] for (int i = 0; i < 4; ++i) {
        float st = steps[i];
        float2 c0 = best;
        tryV(c0 + float2( st, 0), best, bestC);
        tryV(c0 + float2(-st, 0), best, bestC);
        tryV(c0 + float2(0,  st), best, bestC);
        tryV(c0 + float2(0, -st), best, bestC);
        tryV(c0 + float2( st,  st), best, bestC);
        tryV(c0 + float2(-st,  st), best, bestC);
        tryV(c0 + float2( st, -st), best, bestC);
        tryV(c0 + float2(-st, -st), best, bestC);
    }
    gOut[id.xy] = best;
}
