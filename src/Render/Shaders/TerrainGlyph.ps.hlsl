// RenderLayers' glyphs (TerrainLine.vs with GlyphLine): 434120 writes the 16-bit color to each pixel inside the
// font's bounds where the glyph's 1-bit mask is set, without Z. The mask lies in the atlas from the instance's
// texel, nonzero where set.
#ifdef __spirv__
#define COMBINED_IMAGE_SAMPLER [[vk::combinedImageSampler]]
#else
#define COMBINED_IMAGE_SAMPLER
#endif
COMBINED_IMAGE_SAMPLER Texture2D<uint> Atlas : register(t0, space2);
COMBINED_IMAGE_SAMPLER SamplerState AtlasSampler : register(s0, space2);

cbuffer Decode : register(b0, space3)
{
    uint4 Shift;
    uint4 Bits;
    int4 Alpha;
};

struct VSOutput
{
    float4 Position : SV_Position;
    noperspective float Depth : TEXCOORD0;
    nointerpolation uint4 Data : TEXCOORD1;
    nointerpolation int4 Glyph : TEXCOORD2;
};

float4 main(VSOutput input) : SV_Target
{
    const int2 pixel = int2(input.Position.xy);
    const int2 low = int2(input.Data.x & 0xFFFF, input.Data.x >> 16);
    const int2 high = int2(input.Data.y & 0xFFFF, input.Data.y >> 16);
    if (any(pixel < low) || any(pixel >= high))
        discard;
    const uint texel = uint(input.Glyph.z);
    const int2 origin = int2(texel & 0xFFFF, texel >> 16);
    if (Atlas.Load(int3(origin + pixel - input.Glyph.xy, 0)) == 0)
        discard;
    const uint3 maximum = (1u << Bits.xyz) - 1u;
    return float4(float3((input.Data.zzz >> Shift.xyz) & maximum), 1.0);
}
