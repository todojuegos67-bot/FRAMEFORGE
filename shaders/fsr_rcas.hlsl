// AMD FidelityFX FSR 1 RCAS (MIT licence, see THIRD_PARTY.txt)
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"
Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
cbuffer cb : register(b0) { uint4 Const0; };
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return InputTexture.Load(int3(ASU2(p), 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"
void Curr(AU2 pos) {
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, pos, Const0);
    OutputTexture[pos] = AF4(c, 1);
}
[numthreads(64, 1, 1)]
void main(uint3 lt : SV_GroupThreadID, uint3 wg : SV_GroupID) {
    AU2 gxy = ARmp8x8(lt.x) + AU2(wg.x << 4u, wg.y << 4u);
    Curr(gxy); gxy.x += 8u; Curr(gxy); gxy.y += 8u; Curr(gxy); gxy.x -= 8u; Curr(gxy);
}
