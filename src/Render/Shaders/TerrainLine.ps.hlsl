// RenderLayers' lines and fills (TerrainLine.vs): a pixel inside the clip takes the 16-bit color. 4BFD30's
// lines (AlphaLine) skip a pixel whose ABuffer word is 0 and scale each channel by any other word but 127, as
// (channel << spare bits) * A >> 7 >> spare bits; their pipelines test the interpolated depth, which the native
// keeps to 16 bits, strictly smaller and may write it.
#ifdef __spirv__
#define COMBINED_IMAGE_SAMPLER [[vk::combinedImageSampler]]
#else
#define COMBINED_IMAGE_SAMPLER
#endif
COMBINED_IMAGE_SAMPLER Texture2D<float> ABuffer : register(t0, space2);
COMBINED_IMAGE_SAMPLER SamplerState ABufferSampler : register(s0, space2);

cbuffer Decode : register(b0, space3)
{
    uint4 Shift;
    uint4 Bits;
    int4 Alpha; // xy: target position of the ABuffer's first word, zw: its size.
};

struct VSOutput
{
    float4 Position : SV_Position;
    noperspective float Depth : TEXCOORD0;
    nointerpolation uint4 Data : TEXCOORD1;
};

struct PSOutput
{
    float4 Color : SV_Target;
    float Depth : SV_Depth;
};

static const uint AlphaLine = 2;

PSOutput main(VSOutput input)
{
    const int2 pixel = int2(input.Position.xy);
    const int2 low = int2(input.Data.x & 0xFFFF, input.Data.x >> 16);
    const int2 high = int2(input.Data.y & 0xFFFF, input.Data.y >> 16);
    if (any(pixel < low) || any(pixel >= high))
        discard;
    const uint3 maximum = (1u << Bits.xyz) - 1u;
    uint3 color = (input.Data.zzz >> Shift.xyz) & maximum;
    if (input.Data.w & AlphaLine)
    {
        const int2 word = pixel - Alpha.xy;
        uint alpha = 127;
        if (all(word >= 0) && all(word < Alpha.zw))
            alpha = uint(round(ABuffer.Load(int3(word, 0)) * 255.0));
        if (alpha == 0)
            discard;
        if (alpha != 127)
        {
            const uint3 spare = 8u - Bits.xyz;
            color = ((((color << spare) & 0xFF) * alpha) >> 7) >> spare;
        }
    }
    PSOutput output;
    output.Color = float4(float3(color), 1.0);
    output.Depth = float(uint(int(round(input.Depth))) & 0xFFFF) / 65535.0;
    return output;
}
