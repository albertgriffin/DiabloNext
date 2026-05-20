struct VSOutput {
	float4 Position : SV_Position;
	float2 UV : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
	static const uint verts[6] = { 0, 1, 2, 0, 2, 3 };
	static const float2 uvs[4] = {
		float2(0.0, 0.0),
		float2(1.0, 0.0),
		float2(1.0, 1.0),
		float2(0.0, 1.0),
	};
	static const float2 positions[4] = {
		float2(-1.0, 1.0),
		float2(1.0, 1.0),
		float2(1.0, -1.0),
		float2(-1.0, -1.0),
	};

	uint vert = verts[vertexId];

	VSOutput output;
	output.Position = float4(positions[vert], 0.0, 1.0);
	output.UV = uvs[vert];
	return output;
}
