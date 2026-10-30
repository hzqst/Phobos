// TMP tile pixels of 547CF0: the index from the atlas, the shade row from the cell's light row
// and the ABuffer word under the pixel, then the color from the cell's whole LightConvert table.
#ifdef __spirv__
#define COMBINED_IMAGE_SAMPLER [[vk::combinedImageSampler]]
#else
#define COMBINED_IMAGE_SAMPLER
#endif
COMBINED_IMAGE_SAMPLER Texture2D<uint> Atlas : register(t0, space2);
COMBINED_IMAGE_SAMPLER SamplerState AtlasSampler : register(s0, space2);
COMBINED_IMAGE_SAMPLER Texture2D<uint> Tables : register(t1, space2);
COMBINED_IMAGE_SAMPLER SamplerState TableSampler : register(s1, space2);
COMBINED_IMAGE_SAMPLER Texture2D<float> ABuffer : register(t2, space2);
COMBINED_IMAGE_SAMPLER SamplerState ABufferSampler : register(s2, space2);
cbuffer Decode : register(b0, space3)
{
    uint4 Shift;
    uint4 Bits;
    int4 Alpha; // xy: canvas position of the ABuffer's first word, zw: its size.
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 Local : TEXCOORD0;
    nointerpolation uint4 Data : TEXCOORD1;
    nointerpolation uint2 Size : TEXCOORD2;
    nointerpolation int2 Origin : TEXCOORD3;
};

uint2 Unpack(uint value)
{
    return uint2(value & 0xFFFF, value >> 16);
}

// The 60x29 TMP diamond: rows 4, 8, ..., 60, ..., 8, 4 pixels wide, centered.
bool InsideDiamond(uint2 pixel)
{
    const uint half = pixel.y < 15 ? 2 * (pixel.y + 1) : 2 * (29 - pixel.y);
    return pixel.y < 29 && pixel.x + half >= 30 && pixel.x < 30 + half;
}

float4 main(VSOutput input) : SV_Target
{
    const uint2 offset = min(uint2(input.Local), input.Size - 1);
    const uint index = Atlas.Load(int3(Unpack(input.Data.x) + offset, 0));
    // A main diamond draws index 0 too; an extra image leaves it transparent.
    if ((input.Data.w >> 16) != 0 ? !InsideDiamond(Unpack(input.Data.y) + offset) : index == 0)
        discard;
    // The ABuffer word under the pixel; the default 127 outside it.
    const int2 word = input.Origin + int2(offset) - Alpha.xy;
    uint alpha = 127;
    if (all(word >= 0) && all(word < Alpha.zw))
        alpha = uint(round(ABuffer.Load(int3(word, 0)) * 255.0));
    // 420140: min(N-1, (N-1)*q*a / 32258), the low byte as the shade row.
    const uint last = (input.Data.z >> 16) - 1;
    const uint shade = min(last, last * (input.Data.w & 0xFFFF) * alpha / 32258) & 0xFF;
    const uint packed = Tables.Load(int3(index, (input.Data.z & 0xFFFF) + shade, 0));
    const uint3 maximum = (1u << Bits.xyz) - 1u;
    return float4(float3((packed.xxx >> Shift.xyz) & maximum) / float3(maximum), 1.0);
}
