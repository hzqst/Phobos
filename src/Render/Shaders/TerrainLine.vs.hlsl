// RenderLayers' lines, fills and glyphs into the tactical target, one instance each, expanded from SV_VertexID into
// two triangles. A line becomes a parallelogram a pixel across its major axis, from half a pixel before its
// first pixel center to half a pixel past its last, so each column (or row) it crosses covers one pixel center;
// its depth goes linearly between the ends'. This approximates the native Bresenham lines (4BFD30, 7BA610),
// whose pixels may differ at ties and at the ends. A fill or glyph covers its rectangle.

// Matches TerrainBatch::Line.
struct Line
{
	int2 From;
	int2 To;
	int2 Depth;
	uint ClipMin;
	uint ClipMax;
	uint Color;
	uint Flags;
};

StructuredBuffer<Line> Lines : register(t0, space0);

cbuffer View : register(b0, space1)
{
	uint4 View; // x, y: target size; z: first instance of this draw.
};

struct VSOutput
{
	float4 Position : SV_Position;
	noperspective float Depth : TEXCOORD0;
	nointerpolation uint4 Data : TEXCOORD1; // ClipMin, ClipMax, color, flags.
	nointerpolation int4 Glyph : TEXCOORD2; // From, and a glyph's atlas texel (Depth.x).
};

static const uint FilledLine = 1;

VSOutput main(uint vertexId : SV_VertexID)
{
	// Along the line (x: start, end) and across it (y: one side, the other).
	const float2 corners[6] = { float2(0, -1), float2(1, -1), float2(0, 1), float2(0, 1), float2(1, -1), float2(1, 1) };
	const Line segment = Lines[View.z + vertexId / 6];
	const float2 corner = corners[vertexId % 6];
	float2 pixel;
	float depth = 0.0;
	if (segment.Flags & FilledLine)
		pixel = lerp(float2(segment.From), float2(segment.To), float2(corner.x, corner.y * 0.5 + 0.5));
	else
	{
		const float2 delta = float2(segment.To - segment.From);
		const float major = max(abs(delta.x), abs(delta.y));
		// One major-axis pixel per step; a single point steps along X.
		const float2 unit = major > 0.0 ? delta / major : float2(1.0, 0.0);
		const float along = lerp(-0.5, major + 0.5, corner.x);
		const float2 across = abs(unit.x) >= abs(unit.y) ? float2(0.0, 0.5) : float2(0.5, 0.0);
		pixel = float2(segment.From) + 0.5 + unit * along + across * corner.y;
		depth = major > 0.0 ? lerp(float(segment.Depth.x), float(segment.Depth.y), along / major) : float(segment.Depth.x);
	}
	VSOutput output;
	output.Position = float4(pixel / float2(View.xy) * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	output.Depth = depth;
	output.Data = uint4(segment.ClipMin, segment.ClipMax, segment.Color, segment.Flags);
	output.Glyph = int4(segment.From, segment.Depth.x, 0);
	return output;
}
