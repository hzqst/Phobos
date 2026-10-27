// Fullscreen triangle generated from SV_VertexID, drawn without any vertex buffer.
// The viewport selects the letterboxed area of the window.

struct VSOutput
{
	float4 Position : SV_Position;
	float2 TexCoord : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
	VSOutput output;
	output.TexCoord = float2((vertexId << 1) & 2, vertexId & 2);
	output.Position = float4(output.TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return output;
}
