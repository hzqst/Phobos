// One clipped TMP tile image per instance, expanded from SV_VertexID into two triangles.
// The viewport selects the letterboxed area of the window; positions are logical canvas pixels.

// Matches TerrainBatch::Instance; pairs pack X in the low 16 bits.
struct Tile
{
	int2 Position;
	uint Size;
	uint Atlas;
	uint Frame;
	uint Table;
	uint Light;
	uint Reserved;
};

StructuredBuffer<Tile> Tiles : register(t0, space0);

cbuffer View : register(b0, space1)
{
	uint4 View; // x, y: logical canvas size; z: first instance of this draw.
};

struct VSOutput
{
	float4 Position : SV_Position;
	float2 Local : TEXCOORD0; // Pixels into the clipped rectangle.
	nointerpolation uint4 Data : TEXCOORD1; // Atlas texel, frame texel, table, light.
	nointerpolation uint2 Size : TEXCOORD2;
};

VSOutput main(uint vertexId : SV_VertexID)
{
	const uint2 corners[6] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(0, 1), uint2(1, 0), uint2(1, 1) };
	const Tile tile = Tiles[View.z + vertexId / 6];
	const uint2 size = uint2(tile.Size & 0xFFFF, tile.Size >> 16);
	VSOutput output;
	output.Local = float2(corners[vertexId % 6] * size);
	const float2 pixel = float2(tile.Position) + output.Local;
	output.Position = float4(pixel / float2(View.xy) * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	output.Data = uint4(tile.Atlas, tile.Frame, tile.Table, tile.Light);
	output.Size = size;
	return output;
}
