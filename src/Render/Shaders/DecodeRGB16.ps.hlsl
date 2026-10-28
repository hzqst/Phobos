// Decodes the game's 16 bit pixels to RGB. The bit layout (RGB565 or RGB555) comes from the
// game's ColorPackData. The texture is integer, so texels are fetched directly, which also
// makes the scaling nearest neighbour.

// SDL_GPU binds sampled textures as combined image samplers on Vulkan
#ifdef __spirv__
#define COMBINED_IMAGE_SAMPLER [[vk::combinedImageSampler]]
#else
#define COMBINED_IMAGE_SAMPLER
#endif

COMBINED_IMAGE_SAMPLER Texture2D<uint> Source : register(t0, space2);
COMBINED_IMAGE_SAMPLER SamplerState SourceSampler : register(s0, space2);

cbuffer Decode : register(b0, space3)
{
	uint4 Shift; // lowest bit of red, green and blue
	uint4 Bits;  // bit count of red, green and blue
};

float4 main(float4 position : SV_Position, float2 texCoord : TEXCOORD0) : SV_Target
{
	uint width;
	uint height;
	Source.GetDimensions(width, height);

	const uint2 size = uint2(width, height);
	const uint2 texel = min(uint2(texCoord * float2(size)), size - 1);
	const uint value = Source.Load(int3(texel, 0));

	const uint3 maximum = (1u << Bits.xyz) - 1u;
	const uint3 channel = (value.xxx >> Shift.xyz) & maximum;

	return float4(float3(channel) / float3(maximum), 1.0);
}
