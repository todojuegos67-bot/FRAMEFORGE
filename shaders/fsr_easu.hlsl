// AMD FidelityFX FSR 1 EASU (MIT licence, see THIRD_PARTY.txt)
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"
Texture2D<float4> InputTexture : register(t0);
SamplerState samLinearClamp : register(s0);
RWTexture2D<float4> OutputTexture : register(u0);
cbuffer cb : register(b0) { uint4 Const0; uint4 Const1; uint4 Const2; uint4 Const3; };
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return InputTexture.GatherRed(samLinearClamp, p, int2(0, 0)); }
AF4 FsrEasuGF(AF2 p) { return InputTexture.GatherGreen(samLinearClamp, p, int2(0, 0)); }
AF4 FsrEasuBF(AF2 p) { return InputTexture.GatherBlue(samLinearClamp, p, int2(0, 0)); }
#include "ffx_fsr1.h"
void Curr(AU2 pos) {
    AF3 c;
    FsrEasuF(c, pos, Const0, Const1, Const2, Const3);
    OutputTexture[pos] = AF4(c, 1);
}
[numthreads(64, 1, 1)]
void main(uint3 lt : SV_GroupThreadID, uint3 wg : SV_GroupID) {
    AU2 gxy = ARmp8x8(lt.x) + AU2(wg.x << 4u, wg.y << 4u);
    Curr(gxy); gxy.x += 8u; Curr(gxy); gxy.y += 8u; Curr(gxy); gxy.x -= 8u; Curr(gxy);
}
