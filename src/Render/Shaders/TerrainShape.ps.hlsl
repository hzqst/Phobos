// The overlay draws of 4AED70 into the tactical target, after the TMP tiles and smudges: the index
// from the atlas, colored like TerrainTile.ps (the light row with the ABuffer word under the pixel
// selects the shade row of the LightConvert table). The depth follows ShapeDepth: the drawn row's
// base, stepped by the Z gradient, less the signed Z shape byte, tested strictly smaller and written
// by the pipeline. A shadow outputs nothing but alpha 1/2, which its pipeline's blend multiplies
// the target by; the composite's floor keeps the native (c >> 1) & mask. A tinted RLE body ORs the
// tint into the packed color. A translucent one (mode 2's anims) mixes it with the target's color,
// read from a copy taken before its run, in the blitters' integer channel parts; an untested one
// draws whatever its depth.
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
COMBINED_IMAGE_SAMPLER Texture2D<float4> Target : register(t3, space2);
COMBINED_IMAGE_SAMPLER SamplerState TargetSampler : register(s3, space2);

// Matches TerrainBatch::Shape.
struct Shape
{
    int Base;
    uint Accumulator;
    uint RowsPerBand;
    uint BandLimit;
    int BandDelta;
    uint ZOffset;
    uint Flags;
    uint Tint;
};

StructuredBuffer<Shape> Shapes : register(t4, space2);
StructuredBuffer<uint> ZBytes : register(t5, space2);

cbuffer Decode : register(b0, space3)
{
    uint4 Shift;
    uint4 Bits;
    int4 Alpha; // xy: target position of the ABuffer's first word, zw: its size.
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 Local : TEXCOORD0;
    nointerpolation uint4 Data : TEXCOORD1; // Atlas texel, shape index, table, light.
    nointerpolation uint2 Size : TEXCOORD2;
    nointerpolation int2 Origin : TEXCOORD3;
    nointerpolation uint Depth : TEXCOORD4;
};

struct PSOutput
{
    float4 Color : SV_Target;
    float Depth : SV_Depth;
};

static const uint NoZ = 0xFFFFFFFF;
static const uint ShadowShape = 1;
static const uint TranslucentShape = 6;
static const uint UntestedShape = 8;
static const uint RoundedShape = 16;

PSOutput main(VSOutput input)
{
    const uint2 offset = min(uint2(input.Local), input.Size - 1);
    const uint index = Atlas.Load(int3(uint2(input.Data.x & 0xFFFF, input.Data.x >> 16) + offset, 0));
    if (index == 0)
        discard;
    const Shape shape = Shapes[input.Data.y];
    int value = shape.Base + shape.BandDelta * int((shape.Accumulator + offset.y * shape.RowsPerBand) / shape.BandLimit);
    if (shape.ZOffset != NoZ)
    {
        const uint at = shape.ZOffset + offset.y * input.Size.x + offset.x;
        value -= int(((ZBytes[at >> 2] >> ((at & 3) * 8)) & 0xFF) << 24) >> 24;
    }
    // Native: int32 depth < uint16 old. Nothing passes from 65536 on; below 0 every pixel passes,
    // which depth 0 approximates (the native stores the low 16 bits).
    if (value >= 65536 && !(shape.Flags & UntestedShape))
        discard;
    value = min(value, 65535);
    PSOutput output;
    output.Depth = float(max(value, 0)) / 65535.0;
    if (shape.Flags & ShadowShape)
    {
        output.Color = float4(0.0, 0.0, 0.0, 0.5);
        return output;
    }
    const int2 word = input.Origin + int2(offset) - Alpha.xy;
    uint alpha = 127;
    if (all(word >= 0) && all(word < Alpha.zw))
        alpha = uint(round(ABuffer.Load(int3(word, 0)) * 255.0));
    const uint last = (input.Data.z >> 16) - 1;
    const uint shade = min(last, last * (input.Data.w & 0xFFFF) * alpha / 32258) & 0xFF;
    const uint packed = Tables.Load(int3(index, (input.Data.z & 0xFFFF) + shade, 0)) | shape.Tint;
    const uint3 maximum = (1u << Bits.xyz) - 1u;
    uint3 color = (packed.xxx >> Shift.xyz) & maximum;
    const uint translucency = shape.Flags & TranslucentShape;
    if (translucency)
    {
        // (c >> 1) & HalfTranslucencyMask and (c >> 2) & QuatTranslucencyMask halve and quarter each
        // channel; a shadow may have left a half in the target.
        const uint3 old = uint3(floor(Target.Load(int3(int2(input.Position.xy), 0)).rgb));
        if (shape.Flags & RoundedShape)
        {
            // Phobos' patched blitters: the rounded-up mean per channel, again with the incoming
            // color for 25% and with the old one for 75%.
            const uint3 mean = (color + old + 1) >> 1;
            if (translucency == 2)
                color = (mean + color + 1) >> 1;
            else if (translucency == 4)
                color = mean;
            else
                color = (mean + old + 1) >> 1;
        }
        else if (translucency == 2)
            color = 3 * (color >> 2) + (old >> 2);
        else if (translucency == 4)
            color = (color >> 1) + (old >> 1);
        else
            color = (color >> 2) + 3 * (old >> 2);
    }
    output.Color = float4(float3(color), 1.0);
    return output;
}
