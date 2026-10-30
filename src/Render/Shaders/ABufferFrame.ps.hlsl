// Shroud and fog frames into the R8 ABuffer texture, one pass per draw. 47EFE0 overwrites with every
// byte but FE; 47F250 darkens by 127 minus every byte up to 127, which the pipeline's reverse
// subtract applies and the UNORM target clamps at 0.
#ifdef __spirv__
#define COMBINED_IMAGE_SAMPLER [[vk::combinedImageSampler]]
#else
#define COMBINED_IMAGE_SAMPLER
#endif
COMBINED_IMAGE_SAMPLER Texture2D<uint> Atlas : register(t0, space2);
COMBINED_IMAGE_SAMPLER SamplerState AtlasSampler : register(s0, space2);
cbuffer Pass : register(b0, space3)
{
    uint4 Mode; // x: 0 overwrites, 1 darkens.
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 Local : TEXCOORD0;
    nointerpolation uint4 Data : TEXCOORD1;
    nointerpolation uint2 Size : TEXCOORD2;
    nointerpolation int2 Origin : TEXCOORD3;
};

float4 main(VSOutput input) : SV_Target
{
    const uint2 offset = min(uint2(input.Local), input.Size - 1);
    const uint value = Atlas.Load(int3(uint2(input.Data.x & 0xFFFF, input.Data.x >> 16) + offset, 0));
    if (Mode.x == 0 ? value == 0xFE : value > 127)
        discard;
    return float4((Mode.x == 0 ? value : 127 - value) / 255.0, 0.0, 0.0, 1.0);
}
