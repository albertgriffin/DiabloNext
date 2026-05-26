static const unsigned char sdl_gpu_palette_frag_msl[] = R"MSL(#include <metal_stdlib>
using namespace metal;

struct VertexOut
{
	float4 position [[position]];
	float2 uv;
};

struct PaletteUniforms
{
	uint diagnosticView;
	uint smoothLightSourceCount;
	uint lightTextureIsAlpha;
	uint lightTextureIsClassicLevel;
	uint lightTextureIsDungeonGrid;
	uint neutralRectCount;
	uint padding0;
	uint padding1;
	float4 logicalSize;
	float4 classicLightGridWorld;
	float4 classicLightGridScreen;
	float4 neutralRects[8];
	float4 smoothLightSourceGeometry[8];
	float4 smoothLightSourceIntensity[8];
};

constant float PaletteTextureRows = 17.0;
constant float ClassicLightRowOffset = 1.0;

float4 rawPaletteColor(float paletteU, texture2d<float> paletteTexture, sampler paletteSampler)
{
	return paletteTexture.sample(paletteSampler, float2(paletteU, 0.5 / PaletteTextureRows));
}

float4 classicLitPaletteColor(float paletteU, float lightLevel, texture2d<float> paletteTexture, sampler paletteSampler)
{
	float level = clamp(lightLevel, 0.0, 15.0);
	float lowLevel = floor(level);
	float highLevel = ceil(level);
	float blend = level - lowLevel;
	float lowV = (ClassicLightRowOffset + lowLevel + 0.5) / PaletteTextureRows;
	float highV = (ClassicLightRowOffset + highLevel + 0.5) / PaletteTextureRows;
	float4 lowColor = paletteTexture.sample(paletteSampler, float2(paletteU, lowV));
	float4 highColor = paletteTexture.sample(paletteSampler, float2(paletteU, highV));
	return mix(lowColor, highColor, blend);
}

float classicSourceIntensity(float4 geometry, float4 intensity, float2 logicalPosition)
{
	float2 screenDelta = logicalPosition - geometry.xy;
	if (abs(screenDelta.x) > intensity.y || abs(screenDelta.y) > intensity.z) {
		return -1.0;
	}

	float2 lightDelta = float2(2.0 * screenDelta.y + screenDelta.x, 2.0 * screenDelta.y - screenDelta.x) / 8.0;
	float distanceFromSource = length(lightDelta);
	if (distanceFromSource > geometry.z) {
		return -1.0;
	}

	float t = saturate(distanceFromSource / max(1.0, geometry.z));
	return mix(geometry.w, intensity.x, t);
}

float classicLevelToIntensity(float lightLevel)
{
	float level = min(lightLevel, 15.0);
	return 1.0 - level / 15.0;
}

bool classicNeutralPixel(float2 logicalPosition, constant PaletteUniforms& uniforms)
{
	if (logicalPosition.y >= uniforms.classicLightGridScreen.z) {
		return true;
	}
	for (uint i = 0; i < uniforms.neutralRectCount; i++) {
		const float4 rect = uniforms.neutralRects[i];
		if (logicalPosition.x >= rect.x && logicalPosition.x < rect.x + rect.z
			&& logicalPosition.y >= rect.y && logicalPosition.y < rect.y + rect.w) {
			return true;
		}
	}
	return false;
}

float classicGridLevel(float2 logicalPosition, texture2d<float> lightTexture, sampler lightSampler, constant PaletteUniforms& uniforms)
{
	float originX = uniforms.classicLightGridScreen.x + 32.0;
	float originY = uniforms.classicLightGridScreen.y - 16.0;
	float tileX = uniforms.classicLightGridWorld.x + (6.0 - 4.0 * originX - 8.0 * originY + 4.0 * logicalPosition.x + 8.0 * logicalPosition.y) / 256.0;
	float tileY = uniforms.classicLightGridWorld.y + (2.0 + 4.0 * originX - 8.0 * originY - 4.0 * logicalPosition.x + 8.0 * logicalPosition.y) / 256.0;
	if (tileX < 0.0 || tileY < 0.0 || tileX > uniforms.classicLightGridWorld.z - 1.0 || tileY > uniforms.classicLightGridWorld.w - 1.0) {
		return 15.0;
	}

	float2 gridUv = (float2(tileX, tileY) + 0.5) / uniforms.classicLightGridWorld.zw;
	return lightTexture.sample(lightSampler, gridUv).r * 255.0;
}

float3 applySmoothClassicSources(float3 fallbackLight, float sourceMask, float2 uv, constant PaletteUniforms& uniforms)
{
	if (uniforms.smoothLightSourceCount == 0 || sourceMask < 0.5) {
		return fallbackLight;
	}
	float fallbackIntensity = max(fallbackLight.r, max(fallbackLight.g, fallbackLight.b));
	if (fallbackIntensity <= (0.5 / 255.0)) {
		return fallbackLight;
	}

	float2 logicalPosition = uv * uniforms.logicalSize.xy;
	float sourceIntensity = -1.0;
	for (uint i = 0; i < uniforms.smoothLightSourceCount; i++) {
		sourceIntensity = max(sourceIntensity, classicSourceIntensity(uniforms.smoothLightSourceGeometry[i], uniforms.smoothLightSourceIntensity[i], logicalPosition));
	}
	return sourceIntensity >= 0.0 ? float3(sourceIntensity) : fallbackLight;
}

fragment float4 main0(VertexOut in [[stage_in]],
	texture2d<float> indexTexture [[texture(0)]],
	texture2d<float> paletteTexture [[texture(1)]],
	texture2d<float> lightTexture [[texture(2)]],
	texture2d<float> shadowTexture [[texture(3)]],
	sampler indexSampler [[sampler(0)]],
	sampler paletteSampler [[sampler(1)]],
	sampler lightSampler [[sampler(2)]],
	sampler shadowSampler [[sampler(3)]],
	constant PaletteUniforms& uniforms [[buffer(0)]])
{
	const float indexValue = indexTexture.sample(indexSampler, in.uv).r;
	const float paletteIndex = floor(indexValue * 255.0 + 0.5);
	const float paletteU = (paletteIndex + 0.5) / 256.0;
	const float4 color = rawPaletteColor(paletteU, paletteTexture, paletteSampler);
	if (uniforms.lightTextureIsDungeonGrid != 0) {
		const float2 logicalPosition = in.uv * uniforms.logicalSize.xy;
		const bool neutralPixel = classicNeutralPixel(logicalPosition, uniforms);
		const float neutralMask = neutralPixel ? 1.0 : shadowTexture.sample(shadowSampler, in.uv).r;
		const float lightLevel = neutralPixel ? 0.0 : classicGridLevel(logicalPosition, lightTexture, lightSampler, uniforms);
		const float lightIntensity = classicLevelToIntensity(lightLevel);
		if (uniforms.diagnosticView == 1) {
			const float diagnosticIntensity = neutralMask >= 0.5 ? 1.0 : lightIntensity;
			return float4(float3(diagnosticIntensity), 1.0);
		}
		if (uniforms.diagnosticView == 2) {
			return float4(float3(neutralMask), 1.0);
		}
		if (neutralMask >= 0.5) {
			return color;
		}
		return classicLitPaletteColor(paletteU, lightLevel, paletteTexture, paletteSampler);
	}
	const float4 lightSample = lightTexture.sample(lightSampler, in.uv);
	const float classicLightLevel = floor(lightSample.r * 255.0 + 0.5);
	const float scalarLight = uniforms.lightTextureIsClassicLevel != 0 ? classicLevelToIntensity(classicLightLevel) : lightSample.r;
	float3 fallbackLight = uniforms.lightTextureIsAlpha != 0 ? float3(scalarLight) : lightSample.rgb;
	float sourceMask = uniforms.lightTextureIsAlpha != 0 ? 1.0 : lightSample.a;
	if (uniforms.lightTextureIsClassicLevel != 0 && classicLightLevel > 15.0) {
		fallbackLight = float3(1.0);
		sourceMask = 0.0;
	}
	const float3 light = applySmoothClassicSources(fallbackLight, sourceMask, in.uv, uniforms);
	const float shadow = shadowTexture.sample(shadowSampler, in.uv).r;
	if (uniforms.diagnosticView == 1) {
		return float4(light, 1.0);
	}
	if (uniforms.diagnosticView == 2) {
		return float4(float3(shadow), 1.0);
	}
	float3 outputColor = color.rgb * light * (1.0 - shadow);
	return float4(outputColor, color.a);
}
)MSL";
static const unsigned int sdl_gpu_palette_frag_msl_len = sizeof(sdl_gpu_palette_frag_msl) - 1;
