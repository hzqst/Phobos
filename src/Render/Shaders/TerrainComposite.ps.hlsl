// The tactical target onto the window: its channels hold integer values in the color pack's bits,
// floored so a later darkening keeps the native bit truncation, then scaled to unit range.
#ifdef __spirv__
#define COMBINED_IMAGE_SAMPLER [[vk::combinedImageSampler]]
#else
#define COMBINED_IMAGE_SAMPLER
#endif
COMBINED_IMAGE_SAMPLER Texture2D<float4> Tactical : register(t0, space2);
COMBINED_IMAGE_SAMPLER SamplerState TacticalSampler : register(s0, space2);
cbuffer Decode : register(b0, space3)
{
    uint4 Bits;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 Local : TEXCOORD0;
    nointerpolation uint4 Data : TEXCOORD1;
    nointerpolation uint2 Size : TEXCOORD2;
    nointerpolation int2 Origin : TEXCOORD3;
    nointerpolation uint Depth : TEXCOORD4;
};

float4 main(VSOutput input) : SV_Target
{
    const uint2 offset = min(uint2(input.Local), input.Size - 1);
    const float3 value = Tactical.Load(int3(uint2(input.Data.x & 0xFFFF, input.Data.x >> 16) + offset, 0)).rgb;
    const float3 maximum = float3((1u << Bits.xyz) - 1u);
    return float4(floor(value) / maximum, 1.0);
}
