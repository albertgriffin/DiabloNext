Texture2D<float> IndexTexture : register(t0, space2);
Texture2D<float4> PaletteTexture : register(t1, space2);
SamplerState IndexSampler : register(s0, space2);
SamplerState PaletteSampler : register(s1, space2);

struct PSInput {
	float4 Position : SV_Position;
	float2 UV : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
	float indexValue = IndexTexture.Sample(IndexSampler, input.UV).r;
	float paletteIndex = floor(indexValue * 255.0 + 0.5);
	float paletteU = (paletteIndex + 0.5) / 256.0;
	return PaletteTexture.Sample(PaletteSampler, float2(paletteU, 0.5));
}
