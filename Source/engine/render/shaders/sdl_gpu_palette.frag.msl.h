static const unsigned char sdl_gpu_palette_frag_msl[] = R"MSL(#include <metal_stdlib>
using namespace metal;

struct VertexOut
{
	float4 position [[position]];
	float2 uv;
};

fragment float4 main0(VertexOut in [[stage_in]],
	texture2d<float> indexTexture [[texture(0)]],
	texture2d<float> paletteTexture [[texture(1)]],
	texture2d<float> lightTexture [[texture(2)]],
	texture2d<float> shadowTexture [[texture(3)]],
	sampler indexSampler [[sampler(0)]],
	sampler paletteSampler [[sampler(1)]],
	sampler lightSampler [[sampler(2)]],
	sampler shadowSampler [[sampler(3)]])
{
	const float indexValue = indexTexture.sample(indexSampler, in.uv).r;
	const float paletteIndex = floor(indexValue * 255.0 + 0.5);
	const float paletteU = (paletteIndex + 0.5) / 256.0;
	const float4 color = paletteTexture.sample(paletteSampler, float2(paletteU, 0.5));
	const float4 light = lightTexture.sample(lightSampler, in.uv);
	const float shadow = shadowTexture.sample(shadowSampler, in.uv).r;
	return float4(color.rgb * light.rgb * (1.0 - shadow), color.a);
}
)MSL";
static const unsigned int sdl_gpu_palette_frag_msl_len = sizeof(sdl_gpu_palette_frag_msl) - 1;
