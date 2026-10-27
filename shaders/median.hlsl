// 3x3 vector median: keeps a real vector from the neighbourhood, removes outliers.
Texture2D<float2> gIn : register(t0);
RWTexture2D<float2> gOut : register(u0);
cbuffer C : register(b0) { uint2 size; float2 pad; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    float2 v[9];
    int k = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        int2 p = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(size) - 1);
        v[k++] = gIn[p];
    }
    float bestD = 1e30; float2 best = v[4];
    [unroll] for (int i = 0; i < 9; ++i) {
        float d = 0;
        [unroll] for (int j = 0; j < 9; ++j) d += length(v[i] - v[j]);
        if (d < bestD) { bestD = d; best = v[i]; }
    }
    gOut[id.xy] = best;
}
