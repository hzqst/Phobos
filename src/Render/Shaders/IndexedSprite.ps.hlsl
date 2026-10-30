// Original SHP index atlas plus the immutable native 16-bit color mapping.
#ifdef __spirv__
#define COMBINED_IMAGE_SAMPLER [[vk::combinedImageSampler]]
#else
#define COMBINED_IMAGE_SAMPLER
#endif
COMBINED_IMAGE_SAMPLER Texture2D<uint> Source : register(t0, space2);
COMBINED_IMAGE_SAMPLER SamplerState SourceSampler : register(s0, space2);
COMBINED_IMAGE_SAMPLER Texture2D<uint> Palette : register(t1, space2);
COMBINED_IMAGE_SAMPLER SamplerState PaletteSampler : register(s1, space2);
cbuffer Decode : register(b0, space3)
{
    uint4 Shift;
    uint4 Bits;
    uint4 Region;
    uint4 Effect; // x: mode (4 glyph mask, 5 TMP diamond), y: packed color, z: opacity / 256, w: frame origin.
};
// The 60x29 TMP diamond of 547CF0: rows 4, 8, ..., 60, ..., 8, 4 pixels wide, centered.
bool InsideDiamond(uint2 pixel)
{
    const uint half = pixel.y < 15 ? 2 * (pixel.y + 1) : 2 * (29 - pixel.y);
    return pixel.y < 29 && pixel.x + half >= 30 && pixel.x < 30 + half;
}
float4 main(float4 position : SV_Position, float2 texCoord : TEXCOORD0) : SV_Target
{
    const uint3 maximum = (1u << Bits.xyz) - 1u;
    if (Effect.x == 3)
        return float4(float3((Effect.yyy >> Shift.xyz) & maximum) / float3(maximum), min(Effect.z, 256u) / 256.0);
    const uint2 local = min(uint2(texCoord * float2(Region.zw)), Region.zw - 1);
    const uint index = Source.Load(int3(Region.xy + local, 0));
    const uint packed = Effect.x == 4 ? Effect.y : Palette.Load(int3(index, 0, 0));
    const float3 color = Effect.x == 1 ? 0.0 : float3((packed.xxx >> Shift.xyz) & maximum) / float3(maximum);
    if (Effect.x == 5)
        return float4(color, InsideDiamond(Region.xy + local - uint2(Effect.w & 0xFFFF, Effect.w >> 16)) ? 1.0 : 0.0);
    const float alpha = index == 0 ? 0.0 : (Effect.x == 0 || Effect.x == 4 ? 1.0 : 0.5);
    return float4(color, alpha);
}
