Texture2D<float4> gT : register(t0);
SamplerState sLinear : register(s0);
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs(uint id : SV_VertexID) {
    VO o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VO i) : SV_Target { return float4(gT.SampleLevel(sLinear, i.uv, 0).rgb, 1); }
