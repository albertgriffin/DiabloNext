Texture2D<float> IndexTexture : register(t0, space2);
Texture2D<float4> PaletteTexture : register(t1, space2);
Texture2D<float4> LightTexture : register(t2, space2);
Texture2D<float> ShadowTexture : register(t3, space2);
SamplerState IndexSampler : register(s0, space2);
SamplerState PaletteSampler : register(s1, space2);
SamplerState LightSampler : register(s2, space2);
SamplerState ShadowSampler : register(s3, space2);

cbuffer PaletteUniforms : register(b0, space3)
{
	uint DiagnosticView;
	uint Padding0;
	uint Padding1;
	uint Padding2;
};

struct PSInput {
	float4 Position : SV_Position;
	float2 UV : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
	float indexValue = IndexTexture.Sample(IndexSampler, input.UV).r;
	float paletteIndex = floor(indexValue * 255.0 + 0.5);
	float paletteU = (paletteIndex + 0.5) / 256.0;
	float4 color = PaletteTexture.Sample(PaletteSampler, float2(paletteU, 0.5));
	float4 light = LightTexture.Sample(LightSampler, input.UV);
	float shadow = ShadowTexture.Sample(ShadowSampler, input.UV).r;
	if (DiagnosticView == 1) {
		return float4(light.rgb, 1.0);
	}
	if (DiagnosticView == 2) {
		return float4(shadow.xxx, 1.0);
	}
	return float4(color.rgb * light.rgb * (1.0 - shadow), color.a);
}
