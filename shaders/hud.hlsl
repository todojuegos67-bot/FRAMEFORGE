// Static-HUD mask: pixels unchanged for N frames AND with local contrast (edges / text) are
// treated as overlay and passed through unwarped.
Texture2D<float> gPrev : register(t0);
Texture2D<float> gCur : register(t1);
Texture2D<float> gStableIn : register(t2);
RWTexture2D<float> gStableOut : register(u0);
RWTexture2D<float> gHud : register(u1);
cbuffer C : register(b0) { uint2 size; float diffThr; float edgeThr; float frames; uint enabled; float2 pad; };

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    float c = gCur[id.xy];
    float d = abs(c - gPrev[id.xy]);
    float st = d < diffThr ? min(gStableIn[id.xy] + 1.0, 250.0) : 0.0;
    gStableOut[id.xy] = st;
    float mn = c, mx = c;
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x) {
        int2 p = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(size) - 1);
        float v = gCur[p];
        mn = min(mn, v); mx = max(mx, v);
    }
    gHud[id.xy] = (enabled != 0 && st >= frames && (mx - mn) > edgeThr) ? 1.0 : 0.0;
}
