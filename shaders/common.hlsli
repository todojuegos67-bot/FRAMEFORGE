// FrameForge common shader definitions.
SamplerState sLinear : register(s0);
SamplerState sPoint  : register(s1);
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
