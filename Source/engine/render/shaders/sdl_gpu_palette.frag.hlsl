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
	uint SmoothLightSourceCount;
	uint LightTextureIsAlpha;
	uint LightTextureIsClassicLevel;
	uint LightTextureIsDungeonGrid;
	uint NeutralRectCount;
	uint Padding0;
	uint Padding1;
	float4 LogicalSize;
	float4 ClassicLightGridWorld;
	float4 ClassicLightGridScreen;
	float4 NeutralRects[8];
	float4 SmoothLightSourceGeometry[8];
	float4 SmoothLightSourceIntensity[8];
};

struct PSInput {
	float4 Position : SV_Position;
	float2 UV : TEXCOORD0;
};

static const float PaletteTextureRows = 17.0;
static const float ClassicLightRowOffset = 1.0;

float4 RawPaletteColor(float paletteU)
{
	return PaletteTexture.Sample(PaletteSampler, float2(paletteU, 0.5 / PaletteTextureRows));
}

float4 ClassicLitPaletteColor(float paletteU, float lightLevel)
{
	float level = clamp(lightLevel, 0.0, 15.0);
	float lowLevel = floor(level);
	float highLevel = ceil(level);
	float blend = level - lowLevel;
	float lowV = (ClassicLightRowOffset + lowLevel + 0.5) / PaletteTextureRows;
	float highV = (ClassicLightRowOffset + highLevel + 0.5) / PaletteTextureRows;
	float4 lowColor = PaletteTexture.Sample(PaletteSampler, float2(paletteU, lowV));
	float4 highColor = PaletteTexture.Sample(PaletteSampler, float2(paletteU, highV));
	return lerp(lowColor, highColor, blend);
}

float ClassicSourceIntensity(float4 geometry, float4 intensity, float2 logicalPosition)
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
	return lerp(geometry.w, intensity.x, t);
}

float ClassicLevelToIntensity(float lightLevel)
{
	float level = min(lightLevel, 15.0);
	return 1.0 - level / 15.0;
}

bool ClassicNeutralPixel(float2 logicalPosition)
{
	if (logicalPosition.y >= ClassicLightGridScreen.z) {
		return true;
	}
	for (uint i = 0; i < NeutralRectCount; i++) {
		float4 rect = NeutralRects[i];
		if (logicalPosition.x >= rect.x && logicalPosition.x < rect.x + rect.z
			&& logicalPosition.y >= rect.y && logicalPosition.y < rect.y + rect.w) {
			return true;
		}
	}
	return false;
}

float ClassicGridLevel(float2 logicalPosition)
{
	float originX = ClassicLightGridScreen.x + 32.0;
	float originY = ClassicLightGridScreen.y - 16.0;
	float tileX = ClassicLightGridWorld.x + (6.0 - 4.0 * originX - 8.0 * originY + 4.0 * logicalPosition.x + 8.0 * logicalPosition.y) / 256.0;
	float tileY = ClassicLightGridWorld.y + (2.0 + 4.0 * originX - 8.0 * originY - 4.0 * logicalPosition.x + 8.0 * logicalPosition.y) / 256.0;
	if (tileX < 0.0 || tileY < 0.0 || tileX > ClassicLightGridWorld.z - 1.0 || tileY > ClassicLightGridWorld.w - 1.0) {
		return 15.0;
	}

	float2 gridUv = (float2(tileX, tileY) + 0.5) / ClassicLightGridWorld.zw;
	return LightTexture.Sample(LightSampler, gridUv).r * 255.0;
}

float3 ApplySmoothClassicSources(float3 fallbackLight, float sourceMask, float2 uv)
{
	if (SmoothLightSourceCount == 0 || sourceMask < 0.5) {
		return fallbackLight;
	}
	float fallbackIntensity = max(fallbackLight.r, max(fallbackLight.g, fallbackLight.b));
	if (fallbackIntensity <= (0.5 / 255.0)) {
		return fallbackLight;
	}

	float2 logicalPosition = uv * LogicalSize.xy;
	float sourceIntensity = -1.0;
	for (uint i = 0; i < SmoothLightSourceCount; i++) {
		sourceIntensity = max(sourceIntensity, ClassicSourceIntensity(SmoothLightSourceGeometry[i], SmoothLightSourceIntensity[i], logicalPosition));
	}
	return sourceIntensity >= 0.0 ? float3(sourceIntensity, sourceIntensity, sourceIntensity) : fallbackLight;
}

float4 main(PSInput input) : SV_Target
{
	float indexValue = IndexTexture.Sample(IndexSampler, input.UV).r;
	float paletteIndex = floor(indexValue * 255.0 + 0.5);
	float paletteU = (paletteIndex + 0.5) / 256.0;
	float4 color = RawPaletteColor(paletteU);
	if (LightTextureIsDungeonGrid != 0) {
		float2 logicalPosition = input.UV * LogicalSize.xy;
		bool neutralPixel = ClassicNeutralPixel(logicalPosition);
		float neutralMask = neutralPixel ? 1.0 : ShadowTexture.Sample(ShadowSampler, input.UV).r;
		float lightLevel = neutralPixel ? 0.0 : ClassicGridLevel(logicalPosition);
		float lightIntensity = ClassicLevelToIntensity(lightLevel);
		if (DiagnosticView == 1) {
			float diagnosticIntensity = neutralMask >= 0.5 ? 1.0 : lightIntensity;
			return float4(float3(diagnosticIntensity, diagnosticIntensity, diagnosticIntensity), 1.0);
		}
		if (DiagnosticView == 2) {
			return float4(neutralMask.xxx, 1.0);
		}
		if (neutralMask >= 0.5) {
			return color;
		}
		return ClassicLitPaletteColor(paletteU, lightLevel);
	}
	float4 lightSample = LightTexture.Sample(LightSampler, input.UV);
	float classicLightLevel = floor(lightSample.r * 255.0 + 0.5);
	float scalarLight = LightTextureIsClassicLevel != 0 ? ClassicLevelToIntensity(classicLightLevel) : lightSample.r;
	float3 fallbackLight = LightTextureIsAlpha != 0 ? scalarLight.xxx : lightSample.rgb;
	float sourceMask = LightTextureIsAlpha != 0 ? 1.0 : lightSample.a;
	if (LightTextureIsClassicLevel != 0 && classicLightLevel > 15.0) {
		fallbackLight = float3(1.0, 1.0, 1.0);
		sourceMask = 0.0;
	}
	float3 light = ApplySmoothClassicSources(fallbackLight, sourceMask, input.UV);
	float shadow = ShadowTexture.Sample(ShadowSampler, input.UV).r;
	if (DiagnosticView == 1) {
		return float4(light, 1.0);
	}
	if (DiagnosticView == 2) {
		return float4(shadow.xxx, 1.0);
	}
	float3 outputColor = color.rgb * light * (1.0 - shadow);
	return float4(outputColor, color.a);
}
