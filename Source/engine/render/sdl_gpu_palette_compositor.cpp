/**
 * @file sdl_gpu_palette_compositor.cpp
 *
 * Optional SDL_GPU palette compositor target.
 */
#include "engine/render/sdl_gpu_palette_compositor.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>
#endif

#include "controls/control_mode.hpp"
#include "engine/render/accelerated_palette_compositor.hpp"
#include "engine/render/frame_compositor.hpp"
#include "engine/render/render_perf.hpp"
#include "headless_mode.hpp"
#include "lighting.h"
#include "options.h"
#include "utils/log.hpp"

#if defined(DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR) && defined(USE_SDL3) && !defined(USE_SDL1)
#include <SDL3/SDL_gpu.h>
#define DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE 1
#else
#define DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE 0
#endif

#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
#include "engine/lighting_defs.hpp"

#if __has_include("engine/render/shaders/sdl_gpu_palette.frag.spv.h") && __has_include("engine/render/shaders/sdl_gpu_palette.vert.spv.h")
#include "engine/render/shaders/sdl_gpu_palette.frag.spv.h"
#include "engine/render/shaders/sdl_gpu_palette.vert.spv.h"
#define DEVILUTIONX_SDL_GPU_PALETTE_HAS_SPIRV 1
#else
#define DEVILUTIONX_SDL_GPU_PALETTE_HAS_SPIRV 0
#endif

#if __has_include("engine/render/shaders/sdl_gpu_palette.frag.dxil.h") && __has_include("engine/render/shaders/sdl_gpu_palette.vert.dxil.h")
#include "engine/render/shaders/sdl_gpu_palette.frag.dxil.h"
#include "engine/render/shaders/sdl_gpu_palette.vert.dxil.h"
#define DEVILUTIONX_SDL_GPU_PALETTE_HAS_DXIL 1
#else
#define DEVILUTIONX_SDL_GPU_PALETTE_HAS_DXIL 0
#endif

#include "engine/render/shaders/sdl_gpu_palette.frag.msl.h"
#include "engine/render/shaders/sdl_gpu_palette.vert.msl.h"
#endif

namespace devilution {
namespace {

[[nodiscard]] bool SdlGpuPaletteCompositorAllowedByRuntime()
{
#ifdef USE_SDL1
	return false;
#else
	return !HeadlessMode
	    && *GetOptions().Experimental.renderFrameCompositor
	    && *GetOptions().Experimental.renderFrameCompositorBackend == RenderFrameCompositorBackend::SdlGpuPalette
	    && ControlMode != ControlTypes::VirtualGamepad;
#endif
}

#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE

constexpr std::string_view SdlGpuBackendName = "sdl-gpu-palette";

constexpr int PaletteTextureWidth = 256;
constexpr int PaletteClassicLightRowOffset = 1;
constexpr int PaletteTextureHeight = PaletteClassicLightRowOffset + NumLightingLevels;
constexpr uint32_t PaletteUploadBytes = PaletteTextureWidth * PaletteTextureHeight * 4;
constexpr SDL_GPUTextureFormat PaletteExpandedTextureFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr uint32_t PaletteShaderMaxSmoothLightSources = 8;
constexpr uint32_t PaletteShaderMaxNeutralRects = 8;
constexpr size_t AutomapOverlayMaxRejectRects = 4;
constexpr SDL_GPUTextureFormat CursorOverlayTextureFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

constexpr SDL_GPUShaderFormat SupportedShaderFormats = SDL_GPU_SHADERFORMAT_MSL
#if DEVILUTIONX_SDL_GPU_PALETTE_HAS_SPIRV
    | SDL_GPU_SHADERFORMAT_SPIRV
#endif
#if DEVILUTIONX_SDL_GPU_PALETTE_HAS_DXIL
    | SDL_GPU_SHADERFORMAT_DXIL
#endif
    ;

struct PaletteShaderAsset {
	const uint8_t *code = nullptr;
	size_t codeSize = 0;
	const char *entrypoint = nullptr;
	SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_INVALID;
};

struct PaletteShaderUniforms {
	uint32_t diagnosticView = 0;
	uint32_t smoothLightSourceCount = 0;
	uint32_t lightTextureIsAlpha = 0;
	uint32_t lightTextureIsClassicLevel = 0;
	uint32_t lightTextureIsDungeonGrid = 0;
	uint32_t neutralRectCount = 0;
	uint32_t padding0 = 0;
	uint32_t padding1 = 0;
	float logicalSize[4] {};
	float classicLightGridWorld[4] {};
	float classicLightGridScreen[4] {};
	std::array<std::array<float, 4>, PaletteShaderMaxNeutralRects> neutralRects {};
	std::array<std::array<float, 4>, PaletteShaderMaxSmoothLightSources> smoothLightSourceGeometry {};
	std::array<std::array<float, 4>, PaletteShaderMaxSmoothLightSources> smoothLightSourceIntensity {};
};

static_assert(offsetof(PaletteShaderUniforms, logicalSize) % 16 == 0);
static_assert(offsetof(PaletteShaderUniforms, classicLightGridWorld) % 16 == 0);
static_assert(offsetof(PaletteShaderUniforms, classicLightGridScreen) % 16 == 0);
static_assert(offsetof(PaletteShaderUniforms, neutralRects) % 16 == 0);
static_assert(offsetof(PaletteShaderUniforms, smoothLightSourceGeometry) % 16 == 0);
static_assert(offsetof(PaletteShaderUniforms, smoothLightSourceIntensity) % 16 == 0);

struct CursorOverlayShaderUniforms {
	float logicalSize[4] {};
	float rect[4] {};
};

static_assert(offsetof(CursorOverlayShaderUniforms, logicalSize) % 16 == 0);
static_assert(offsetof(CursorOverlayShaderUniforms, rect) % 16 == 0);

struct AutomapOverlayShaderUniforms {
	float logicalSize[4] {};
	float offset[4] {};
	std::array<std::array<float, 4>, AutomapOverlayMaxRejectRects> rejectRects {};
	float rejectRectCount[4] {};
};

static_assert(offsetof(AutomapOverlayShaderUniforms, logicalSize) % 16 == 0);
static_assert(offsetof(AutomapOverlayShaderUniforms, offset) % 16 == 0);
static_assert(offsetof(AutomapOverlayShaderUniforms, rejectRects) % 16 == 0);
static_assert(offsetof(AutomapOverlayShaderUniforms, rejectRectCount) % 16 == 0);

struct AutomapOverlayVertex {
	float x = 0.F;
	float y = 0.F;
	float r = 0.F;
	float g = 0.F;
	float b = 0.F;
	float a = 0.F;
};

static const unsigned char sdl_gpu_cursor_overlay_vert_msl[] = R"MSL(#include <metal_stdlib>
using namespace metal;

struct VertexOut
{
	float4 position [[position]];
	float2 uv;
};

struct CursorOverlayUniforms
{
	float4 logicalSize;
	float4 rect;
};

vertex VertexOut main0(uint vertexId [[vertex_id]], constant CursorOverlayUniforms& uniforms [[buffer(0)]])
{
	constexpr uint verts[6] = { 0, 1, 2, 0, 2, 3 };
	constexpr float2 uvs[4] = {
		float2(0.0, 0.0),
		float2(1.0, 0.0),
		float2(1.0, 1.0),
		float2(0.0, 1.0),
	};

	const uint vert = verts[vertexId];
	const float2 uv = uvs[vert];
	const float2 logicalSize = max(uniforms.logicalSize.xy, float2(1.0, 1.0));
	const float2 logicalPosition = uniforms.rect.xy + uv * uniforms.rect.zw;
	const float2 ndc = float2(
		logicalPosition.x / logicalSize.x * 2.0 - 1.0,
		1.0 - logicalPosition.y / logicalSize.y * 2.0);

	VertexOut out;
	out.position = float4(ndc, 0.0, 1.0);
	out.uv = uv;
	return out;
}
)MSL";
static const unsigned int sdl_gpu_cursor_overlay_vert_msl_len = sizeof(sdl_gpu_cursor_overlay_vert_msl) - 1;

static const unsigned char sdl_gpu_cursor_overlay_frag_msl[] = R"MSL(#include <metal_stdlib>
using namespace metal;

struct VertexOut
{
	float4 position [[position]];
	float2 uv;
};

fragment float4 main0(VertexOut in [[stage_in]], texture2d<float> cursorTexture [[texture(0)]], sampler cursorSampler [[sampler(0)]])
{
	return cursorTexture.sample(cursorSampler, in.uv);
}
)MSL";
static const unsigned int sdl_gpu_cursor_overlay_frag_msl_len = sizeof(sdl_gpu_cursor_overlay_frag_msl) - 1;

static const unsigned char sdl_gpu_automap_overlay_vert_msl[] = R"MSL(#include <metal_stdlib>
using namespace metal;

struct VertexIn
{
	float2 position [[attribute(0)]];
	float4 color [[attribute(1)]];
};

struct VertexOut
{
	float4 position [[position]];
	float4 color;
	float2 logicalPosition;
};

struct AutomapOverlayUniforms
{
	float4 logicalSize;
	float4 offset;
	float4 rejectRects[4];
	float4 rejectRectCount;
};

vertex VertexOut main0(VertexIn in [[stage_in]], constant AutomapOverlayUniforms& uniforms [[buffer(0)]])
{
	const float2 logicalSize = max(uniforms.logicalSize.xy, float2(1.0, 1.0));
	const float2 logicalPosition = in.position + uniforms.offset.xy;
	const float2 ndc = float2(
		logicalPosition.x / logicalSize.x * 2.0 - 1.0,
		1.0 - logicalPosition.y / logicalSize.y * 2.0);

	VertexOut out;
	out.position = float4(ndc, 0.0, 1.0);
	out.color = in.color;
	out.logicalPosition = logicalPosition;
	return out;
}
)MSL";
static const unsigned int sdl_gpu_automap_overlay_vert_msl_len = sizeof(sdl_gpu_automap_overlay_vert_msl) - 1;

static const unsigned char sdl_gpu_automap_overlay_frag_msl[] = R"MSL(#include <metal_stdlib>
using namespace metal;

struct VertexOut
{
	float4 position [[position]];
	float4 color;
	float2 logicalPosition;
};

struct AutomapOverlayUniforms
{
	float4 logicalSize;
	float4 offset;
	float4 rejectRects[4];
	float4 rejectRectCount;
};

fragment float4 main0(VertexOut in [[stage_in]], constant AutomapOverlayUniforms& uniforms [[buffer(0)]])
{
	const uint rejectRectCount = min(uint(uniforms.rejectRectCount.x + 0.5), 4u);
	for (uint i = 0; i < rejectRectCount; i++) {
		const float4 rect = uniforms.rejectRects[i];
		if (in.logicalPosition.x >= rect.x && in.logicalPosition.x < rect.z
		    && in.logicalPosition.y >= rect.y && in.logicalPosition.y < rect.w) {
			discard_fragment();
		}
	}
	return in.color;
}
)MSL";
static const unsigned int sdl_gpu_automap_overlay_frag_msl_len = sizeof(sdl_gpu_automap_overlay_frag_msl) - 1;

[[nodiscard]] uint32_t PaletteShaderDiagnosticView(const RenderLightShadowDiagnosticMode mode)
{
	switch (mode) {
	case RenderLightShadowDiagnosticMode::LightRgb:
		return 1;
	case RenderLightShadowDiagnosticMode::ShadowAlpha:
		return 2;
	case RenderLightShadowDiagnosticMode::Off:
	case RenderLightShadowDiagnosticMode::FinalLitOutput:
		break;
	}
	return 0;
}

[[nodiscard]] float ClassicLightIntensityFactor(const uint8_t lightLevel)
{
	const int level = std::min<int>(lightLevel, LightsMax);
	return 1.0F - static_cast<float>(level) / static_cast<float>(LightsMax);
}

void PopulatePaletteShaderSmoothLightSources(PaletteShaderUniforms &uniforms, const CompositionLightingInputs *lightingInputs, const Size logicalSize)
{
	uniforms.logicalSize[0] = static_cast<float>(std::max(1, logicalSize.width));
	uniforms.logicalSize[1] = static_cast<float>(std::max(1, logicalSize.height));

	if (lightingInputs == nullptr || !lightingInputs->smoothPresentation || lightingInputs->smoothLightSources.sources == nullptr)
		return;

	const size_t count = std::min<size_t>(lightingInputs->smoothLightSources.count, PaletteShaderMaxSmoothLightSources);
	uniforms.smoothLightSourceCount = static_cast<uint32_t>(count);
	for (size_t i = 0; i < count; i++) {
		const RenderSmoothLightSource &source = lightingInputs->smoothLightSources.sources[i];
		const float maxDistance = std::max(1.0F, static_cast<float>(source.radius + 1) * 8.0F);
		uniforms.smoothLightSourceGeometry[i] = {
			static_cast<float>(source.screenPosition.x),
			static_cast<float>(source.screenPosition.y),
			maxDistance,
			ClassicLightIntensityFactor(source.centerLightLevel),
		};
		uniforms.smoothLightSourceIntensity[i] = {
			ClassicLightIntensityFactor(source.edgeLightLevel),
			maxDistance * 5.657F + 1.0F,
			maxDistance * 2.829F + 1.0F,
			0.0F,
		};
	}
}

[[nodiscard]] const char *ShaderFormatName(const SDL_GPUShaderFormat format)
{
	switch (format) {
	case SDL_GPU_SHADERFORMAT_SPIRV:
		return "SPIR-V";
	case SDL_GPU_SHADERFORMAT_DXBC:
		return "DXBC";
	case SDL_GPU_SHADERFORMAT_DXIL:
		return "DXIL";
	case SDL_GPU_SHADERFORMAT_MSL:
		return "MSL";
	case SDL_GPU_SHADERFORMAT_METALLIB:
		return "MetalLib";
	default:
		return "unknown";
	}
}

[[nodiscard]] PaletteShaderAsset SelectPaletteVertexShader(const SDL_GPUShaderFormat supportedFormats)
{
#if DEVILUTIONX_SDL_GPU_PALETTE_HAS_SPIRV
	if ((supportedFormats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
		return { sdl_gpu_palette_vert_spv, sdl_gpu_palette_vert_spv_len, "main", SDL_GPU_SHADERFORMAT_SPIRV };
	}
#endif
#if DEVILUTIONX_SDL_GPU_PALETTE_HAS_DXIL
	if ((supportedFormats & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
		return { sdl_gpu_palette_vert_dxil, sdl_gpu_palette_vert_dxil_len, "main", SDL_GPU_SHADERFORMAT_DXIL };
	}
#endif
	if ((supportedFormats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
		return { sdl_gpu_palette_vert_msl, sdl_gpu_palette_vert_msl_len, "main0", SDL_GPU_SHADERFORMAT_MSL };
	}
	return {};
}

[[nodiscard]] PaletteShaderAsset SelectPaletteFragmentShader(const SDL_GPUShaderFormat supportedFormats)
{
#if DEVILUTIONX_SDL_GPU_PALETTE_HAS_SPIRV
	if ((supportedFormats & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
		return { sdl_gpu_palette_frag_spv, sdl_gpu_palette_frag_spv_len, "main", SDL_GPU_SHADERFORMAT_SPIRV };
	}
#endif
#if DEVILUTIONX_SDL_GPU_PALETTE_HAS_DXIL
	if ((supportedFormats & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
		return { sdl_gpu_palette_frag_dxil, sdl_gpu_palette_frag_dxil_len, "main", SDL_GPU_SHADERFORMAT_DXIL };
	}
#endif
	if ((supportedFormats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
		return { sdl_gpu_palette_frag_msl, sdl_gpu_palette_frag_msl_len, "main0", SDL_GPU_SHADERFORMAT_MSL };
	}
	return {};
}

[[nodiscard]] PaletteShaderAsset SelectCursorOverlayVertexShader(const SDL_GPUShaderFormat supportedFormats)
{
	if ((supportedFormats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
		return { sdl_gpu_cursor_overlay_vert_msl, sdl_gpu_cursor_overlay_vert_msl_len, "main0", SDL_GPU_SHADERFORMAT_MSL };
	}
	return {};
}

[[nodiscard]] PaletteShaderAsset SelectCursorOverlayFragmentShader(const SDL_GPUShaderFormat supportedFormats)
{
	if ((supportedFormats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
		return { sdl_gpu_cursor_overlay_frag_msl, sdl_gpu_cursor_overlay_frag_msl_len, "main0", SDL_GPU_SHADERFORMAT_MSL };
	}
	return {};
}

[[nodiscard]] PaletteShaderAsset SelectAutomapOverlayVertexShader(const SDL_GPUShaderFormat supportedFormats)
{
	if ((supportedFormats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
		return { sdl_gpu_automap_overlay_vert_msl, sdl_gpu_automap_overlay_vert_msl_len, "main0", SDL_GPU_SHADERFORMAT_MSL };
	}
	return {};
}

[[nodiscard]] PaletteShaderAsset SelectAutomapOverlayFragmentShader(const SDL_GPUShaderFormat supportedFormats)
{
	if ((supportedFormats & SDL_GPU_SHADERFORMAT_MSL) != 0) {
		return { sdl_gpu_automap_overlay_frag_msl, sdl_gpu_automap_overlay_frag_msl_len, "main0", SDL_GPU_SHADERFORMAT_MSL };
	}
	return {};
}

[[nodiscard]] int SurfaceBytesPerPixel(const SDL_Surface &surface)
{
	return SDL_BYTESPERPIXEL(surface.format);
}

[[nodiscard]] int AttachmentBytesPerPixel(const CompositionAttachmentFormat format)
{
	switch (format) {
	case CompositionAttachmentFormat::Index8:
	case CompositionAttachmentFormat::Alpha8:
		return 1;
	case CompositionAttachmentFormat::PaletteRgba8:
	case CompositionAttachmentFormat::Rgba8:
		return 4;
	case CompositionAttachmentFormat::Unknown:
		break;
	}
	return 0;
}

[[nodiscard]] uint32_t AttachmentTransferPixelsPerRow(const CompositionAttachment &attachment)
{
	const int bytesPerPixel = AttachmentBytesPerPixel(attachment.format);
	if (bytesPerPixel <= 0)
		return 0;
	return static_cast<uint32_t>(attachment.pitch / bytesPerPixel);
}

[[nodiscard]] uint64_t AttachmentTransferBufferSize(const CompositionAttachment &attachment)
{
	if (attachment.pitch <= 0 || attachment.logicalSize.height <= 0)
		return 0;
	return static_cast<uint64_t>(attachment.pitch) * static_cast<uint64_t>(attachment.logicalSize.height);
}

[[nodiscard]] CompositionAttachmentFormat AttachmentFormatForLightingBuffer(const CompositionLightingBufferFormat format)
{
	switch (format) {
	case CompositionLightingBufferFormat::Rgba8:
		return CompositionAttachmentFormat::Rgba8;
	case CompositionLightingBufferFormat::Alpha8:
		return CompositionAttachmentFormat::Alpha8;
	}
	return CompositionAttachmentFormat::Unknown;
}

[[nodiscard]] SDL_GPUTextureFormat GpuTextureFormatForLightingBuffer(const CompositionLightingBufferFormat format)
{
	switch (format) {
	case CompositionLightingBufferFormat::Rgba8:
		return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	case CompositionLightingBufferFormat::Alpha8:
		return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
	}
	return SDL_GPU_TEXTUREFORMAT_INVALID;
}

[[nodiscard]] CompositionAttachment MakeLightingAttachment(const CompositionAttachmentRole role, const CompositionLightingBufferView &view)
{
	return {
		role,
		AttachmentFormatForLightingBuffer(view.format),
		view.size,
		view.pitch,
		view.version,
		view.dirtyRects,
		view.pixels,
	};
}

void RecordSuccessfulUpload(RenderPerfCompositionStats &stats, const CompositionAttachmentUploadPlan &plan)
{
	if (plan.action == CompositionAttachmentUploadAction::Skip) {
		stats.skippedUploadCount++;
		return;
	}
	stats.uploadBytes += plan.byteCount;
	stats.uploadedRectCount += static_cast<uint32_t>(plan.rects.size());
	if (plan.action == CompositionAttachmentUploadAction::Full)
		stats.fullUploadCount++;
}

[[nodiscard]] bool FailUpload(RenderPerfCompositionStats &stats, const CompositionUploadFallbackReason reason)
{
	stats.failedUploadCount++;
	stats.uploadFallbackReason = reason;
	return false;
}

void ReadSurfaceRgba(const SDL_Surface &surface, const uint32_t pixel, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a)
{
	SDL_GetRGBA(pixel, SDL_GetPixelFormatDetails(surface.format), SDL_GetSurfacePalette(const_cast<SDL_Surface *>(&surface)), &r, &g, &b, &a);
}

class SdlGpuPaletteCompositorState {
public:
	bool Initialize(SDL_Window *window)
	{
		Destroy();
		if (window == nullptr)
			return false;

		device_ = SDL_CreateGPUDevice(SupportedShaderFormats, false, nullptr);
		if (device_ == nullptr) {
			Log("SDL_GPU palette compositor unavailable: {}", SDL_GetError());
			return false;
		}
		window_ = window;
		if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
			Log("SDL_GPU palette compositor unavailable: {}", SDL_GetError());
			Destroy();
			return false;
		}

		windowClaimed_ = true;
		UpdateSwapchainParameters();
		Log("SDL_GPU palette compositor initialized using {}", SDL_GetGPUDeviceDriver(device_));
		available_ = true;
		return true;
	}

	void Destroy()
	{
		ReleaseAutomapOverlayResources();
		ReleaseCursorOverlayResources();
		ReleasePalettePipelineResources();
		ReleasePaletteExpandedTexture();
		ReleaseTexture();
		ReleaseIndexedTextures();
		ReleaseLightingTextures();
		ReleaseTransferBuffer();
		ReleaseIndexedTransferBuffers();
		ReleaseLightingTransferBuffers();
		if (windowClaimed_ && device_ != nullptr && window_ != nullptr)
			SDL_ReleaseWindowFromGPUDevice(device_, window_);
		if (device_ != nullptr)
			SDL_DestroyGPUDevice(device_);
		device_ = nullptr;
		window_ = nullptr;
		windowClaimed_ = false;
		available_ = false;
		outputTextureWidth_ = 0;
		outputTextureHeight_ = 0;
		indexTextureWidth_ = 0;
		indexTextureHeight_ = 0;
		transferBufferSize_ = 0;
		indexTransferBufferSize_ = 0;
		lastPresentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
		presentModeInitialized_ = false;
		loggedIndexedDeferral_ = false;
		loggedIndexedPresentation_ = false;
		loggedLightingInputs_ = false;
		loggedCursorOverlay_ = false;
		loggedAutomapOverlay_ = false;
		loggedDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
		palettePipelineFailed_ = false;
		cursorOverlayPipelineFailed_ = false;
		automapOverlayPipelineFailed_ = false;
		indexTextureUploaded_ = false;
		paletteTextureUploaded_ = false;
		lightTextureUploaded_ = false;
		shadowTextureUploaded_ = false;
		uploadedIndexVersion_ = 0;
		uploadedPaletteVersion_ = 0;
		uploadedLightVersion_ = 0;
		uploadedShadowVersion_ = 0;
		pendingMode_ = PendingMode::None;
		pendingLightShadowDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
		pendingSmoothLightingPresentation_ = false;
		pendingPaletteShaderUniforms_ = {};
		palettePipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
		paletteExpandedTextureWidth_ = 0;
		paletteExpandedTextureHeight_ = 0;
		cursorOverlayTextureWidth_ = 0;
		cursorOverlayTextureHeight_ = 0;
		cursorOverlayTransferBufferSize_ = 0;
		automapOverlayVertexBufferSize_ = 0;
		automapOverlayTransferBufferSize_ = 0;
		automapPlayerOverlayVertexBufferSize_ = 0;
		automapPlayerOverlayTransferBufferSize_ = 0;
		cursorOverlayTextureUploaded_ = false;
		automapOverlayVertexBufferUploaded_ = false;
		automapPlayerOverlayVertexBufferUploaded_ = false;
		uploadedCursorOverlayVersion_ = 0;
		uploadedAutomapOverlayVersion_ = 0;
		uploadedAutomapOverlayPaletteVersion_ = 0;
		uploadedAutomapPlayerOverlayVersion_ = 0;
		uploadedAutomapPlayerOverlayPaletteVersion_ = 0;
		cursorOverlayVisible_ = false;
		automapOverlayVisible_ = false;
		automapPlayerOverlayVisible_ = false;
		cursorOverlayPosition_ = {};
		cursorOverlaySize_ = {};
		cursorOverlayVersion_ = 0;
		automapOverlayVersion_ = 0;
		automapPlayerOverlayVersion_ = 0;
		automapOverlayInputVersion_ = 0;
		automapPlayerOverlayInputVersion_ = 0;
		automapOverlayOffset_ = {};
		automapOverlayVertexCount_ = 0;
		automapPlayerOverlayVertexCount_ = 0;
		automapOverlayRejectRectCount_ = 0;
		lightTextureWidth_ = 0;
		lightTextureHeight_ = 0;
		shadowTextureWidth_ = 0;
		shadowTextureHeight_ = 0;
		pendingLogicalSize_ = {};
		pendingOutputSize_ = {};
		outputRgba_.clear();
		cursorOverlayPixels_.clear();
		automapOverlayRects_.clear();
		automapOverlayVertices_.clear();
		automapPlayerOverlayRects_.clear();
		automapPlayerOverlayVertices_.clear();
	}

	[[nodiscard]] bool IsAvailable() const
	{
		return available_;
	}

	[[nodiscard]] bool GpuCursorOverlayAvailable() const
	{
		if (!available_ || device_ == nullptr || cursorOverlayPipelineFailed_)
			return false;
		return (SDL_GetGPUShaderFormats(device_) & SDL_GPU_SHADERFORMAT_MSL) != 0;
	}

	[[nodiscard]] bool GpuAutomapOverlayAvailable() const
	{
		if (!available_ || device_ == nullptr || automapOverlayPipelineFailed_)
			return false;
		return (SDL_GetGPUShaderFormats(device_) & SDL_GPU_SHADERFORMAT_MSL) != 0;
	}

	void SetCursorOverlay(const SdlGpuPaletteCursorOverlay &overlay)
	{
		if (!overlay.visible || overlay.rgbaPixels == nullptr || overlay.size.width <= 0 || overlay.size.height <= 0 || overlay.pitch < overlay.size.width * 4) {
			ClearCursorOverlay();
			return;
		}
		if (!loggedCursorOverlay_) {
			Log("SDL_GPU palette compositor using GPU-composited cursor overlay");
			loggedCursorOverlay_ = true;
		}

		if (cursorOverlayVisible_ && cursorOverlayVersion_ == overlay.version && cursorOverlaySize_ == overlay.size) {
			cursorOverlayPosition_ = overlay.position;
			return;
		}

		const size_t rowBytes = static_cast<size_t>(overlay.size.width) * 4;
		cursorOverlayPixels_.resize(rowBytes * static_cast<size_t>(overlay.size.height));
		for (int y = 0; y < overlay.size.height; y++) {
			std::memcpy(
			    cursorOverlayPixels_.data() + static_cast<size_t>(y) * rowBytes,
			    overlay.rgbaPixels + static_cast<ptrdiff_t>(y) * overlay.pitch,
			    rowBytes);
		}
		cursorOverlayVisible_ = true;
		cursorOverlayPosition_ = overlay.position;
		cursorOverlaySize_ = overlay.size;
		cursorOverlayVersion_ = overlay.version;
	}

	void ClearCursorOverlay()
	{
		cursorOverlayVisible_ = false;
		cursorOverlayPosition_ = {};
		cursorOverlaySize_ = {};
		cursorOverlayVersion_ = 0;
	}

	void SetAutomapOverlay(const RenderAutomapOverlayView overlay)
	{
		if (!overlay.active || overlay.rects == nullptr || overlay.rectCount == 0) {
			ClearAutomapOverlay();
			return;
		}
		if (!loggedAutomapOverlay_) {
			Log("SDL_GPU palette compositor using GPU-composited automap overlay");
			loggedAutomapOverlay_ = true;
		}
		if (automapOverlayVisible_ && overlay.version != 0 && automapOverlayInputVersion_ == overlay.version)
			return;
		if (automapOverlayVisible_ && AutomapOverlayRectsEqual(overlay)) {
			automapOverlayInputVersion_ = overlay.version;
			return;
		}
		automapOverlayRects_.assign(overlay.rects, overlay.rects + overlay.rectCount);
		automapOverlayVisible_ = true;
		automapOverlayInputVersion_ = overlay.version;
		automapOverlayVersion_++;
		if (automapOverlayVersion_ == 0)
			automapOverlayVersion_ = 1;
	}

	void SetAutomapOverlayOffset(const Displacement offset)
	{
		automapOverlayOffset_ = offset;
	}

	void SetAutomapOverlayRejectRects(const Rectangle *rects, const size_t count)
	{
		automapOverlayRejectRectCount_ = 0;
		if (rects == nullptr)
			return;

		for (size_t i = 0; i < count && automapOverlayRejectRectCount_ < AutomapOverlayMaxRejectRects; i++) {
			if (rects[i].size.width <= 0 || rects[i].size.height <= 0)
				continue;
			automapOverlayRejectRects_[automapOverlayRejectRectCount_] = rects[i];
			automapOverlayRejectRectCount_++;
		}
	}

	void SetAutomapPlayerOverlay(const RenderAutomapOverlayView overlay)
	{
		if (!overlay.active || overlay.rects == nullptr || overlay.rectCount == 0) {
			ClearAutomapPlayerOverlay();
			return;
		}
		if (automapPlayerOverlayVisible_ && overlay.version != 0 && automapPlayerOverlayInputVersion_ == overlay.version)
			return;
		if (automapPlayerOverlayVisible_ && AutomapPlayerOverlayRectsEqual(overlay)) {
			automapPlayerOverlayInputVersion_ = overlay.version;
			return;
		}
		automapPlayerOverlayRects_.assign(overlay.rects, overlay.rects + overlay.rectCount);
		automapPlayerOverlayVisible_ = true;
		automapPlayerOverlayInputVersion_ = overlay.version;
		automapPlayerOverlayVersion_++;
		if (automapPlayerOverlayVersion_ == 0)
			automapPlayerOverlayVersion_ = 1;
	}

	void ClearAutomapOverlay()
	{
		automapOverlayVisible_ = false;
		automapOverlayVersion_ = 0;
		automapOverlayInputVersion_ = 0;
		automapOverlayOffset_ = {};
		automapOverlayVertexCount_ = 0;
		automapOverlayVertexBufferUploaded_ = false;
		uploadedAutomapOverlayVersion_ = 0;
		uploadedAutomapOverlayPaletteVersion_ = 0;
		automapOverlayRejectRectCount_ = 0;
		automapOverlayRects_.clear();
		automapOverlayVertices_.clear();
		ClearAutomapPlayerOverlay();
	}

	void ClearAutomapPlayerOverlay()
	{
		automapPlayerOverlayVisible_ = false;
		automapPlayerOverlayVersion_ = 0;
		automapPlayerOverlayInputVersion_ = 0;
		automapPlayerOverlayVertexCount_ = 0;
		automapPlayerOverlayVertexBufferUploaded_ = false;
		uploadedAutomapPlayerOverlayVersion_ = 0;
		uploadedAutomapPlayerOverlayPaletteVersion_ = 0;
		automapPlayerOverlayRects_.clear();
		automapPlayerOverlayVertices_.clear();
	}

	[[nodiscard]] bool AutomapOverlayRectsEqual(const RenderAutomapOverlayView overlay) const
	{
		if (automapOverlayRects_.size() != overlay.rectCount)
			return false;
		for (size_t i = 0; i < overlay.rectCount; i++) {
			const RenderAutomapOverlayRect &left = automapOverlayRects_[i];
			const RenderAutomapOverlayRect &right = overlay.rects[i];
			if (left.colorIndex != right.colorIndex || left.alpha != right.alpha)
				return false;
			if (left.rect.position.x != right.rect.position.x || left.rect.position.y != right.rect.position.y)
				return false;
			if (left.rect.size.width != right.rect.size.width || left.rect.size.height != right.rect.size.height)
				return false;
		}
		return true;
	}

	[[nodiscard]] bool AutomapPlayerOverlayRectsEqual(const RenderAutomapOverlayView overlay) const
	{
		if (automapPlayerOverlayRects_.size() != overlay.rectCount)
			return false;
		for (size_t i = 0; i < overlay.rectCount; i++) {
			const RenderAutomapOverlayRect &left = automapPlayerOverlayRects_[i];
			const RenderAutomapOverlayRect &right = overlay.rects[i];
			if (left.colorIndex != right.colorIndex || left.alpha != right.alpha)
				return false;
			if (left.rect.position.x != right.rect.position.x || left.rect.position.y != right.rect.position.y)
				return false;
			if (left.rect.size.width != right.rect.size.width || left.rect.size.height != right.rect.size.height)
				return false;
		}
		return true;
	}

	[[nodiscard]] bool PrepareIndexedFrame(const AcceleratedPaletteFrame &frame, RenderPerfCompositionStats &stats)
	{
		const CompositionFrame &composition = frame.composition;
		if (!EnsurePaletteRenderResources()
		    || !EnsurePaletteExpandedTexture(composition.logicalSize.width, composition.logicalSize.height)
		    || !UploadIndexedInputs(frame, stats)) {
			if (!loggedIndexedDeferral_) {
				Log("SDL_GPU palette compositor could not prepare indexed palette presentation; using CPU pixel upload fallback");
				loggedIndexedDeferral_ = true;
			}
			return false;
		}

		if (!loggedIndexedPresentation_) {
			Log("SDL_GPU palette compositor using indexed palette shader presentation");
			loggedIndexedPresentation_ = true;
		}
		if (!loggedLightingInputs_) {
			Log("SDL_GPU palette compositor using light/shadow shader inputs");
			loggedLightingInputs_ = true;
		}
		const RenderLightShadowDiagnosticMode diagnosticMode = frame.lighting != nullptr ? frame.lighting->diagnosticMode : RenderLightShadowDiagnosticMode::Off;
		if (diagnosticMode != RenderLightShadowDiagnosticMode::Off && loggedDiagnosticMode_ != diagnosticMode) {
			Log("SDL_GPU palette compositor consuming light/shadow diagnostic: {}", RenderLightShadowDiagnosticModeName(diagnosticMode));
			loggedDiagnosticMode_ = diagnosticMode;
		}
		pendingLogicalSize_ = composition.logicalSize;
		pendingOutputSize_ = {};
		pendingLightShadowDiagnosticMode_ = diagnosticMode;
		pendingSmoothLightingPresentation_ = frame.lighting != nullptr && frame.lighting->smoothPresentation;
		pendingPaletteShaderUniforms_ = {};
		pendingPaletteShaderUniforms_.diagnosticView = PaletteShaderDiagnosticView(diagnosticMode);
		pendingPaletteShaderUniforms_.lightTextureIsAlpha = frame.lighting != nullptr && frame.lighting->light.format == CompositionLightingBufferFormat::Alpha8 ? 1 : 0;
		pendingPaletteShaderUniforms_.lightTextureIsClassicLevel = frame.lighting != nullptr && frame.lighting->lightStoresClassicLightLevels ? 1 : 0;
		pendingPaletteShaderUniforms_.lightTextureIsDungeonGrid = frame.lighting != nullptr && frame.lighting->lightStoresDungeonGrid ? 1 : 0;
		if (frame.lighting != nullptr && frame.lighting->lightStoresDungeonGrid) {
			pendingPaletteShaderUniforms_.classicLightGridWorld[0] = static_cast<float>(frame.lighting->classicLightFirstTile.x);
			pendingPaletteShaderUniforms_.classicLightGridWorld[1] = static_cast<float>(frame.lighting->classicLightFirstTile.y);
			pendingPaletteShaderUniforms_.classicLightGridWorld[2] = static_cast<float>(frame.lighting->light.size.width);
			pendingPaletteShaderUniforms_.classicLightGridWorld[3] = static_cast<float>(frame.lighting->light.size.height);
			pendingPaletteShaderUniforms_.classicLightGridScreen[0] = static_cast<float>(frame.lighting->classicLightOffset.deltaX);
			pendingPaletteShaderUniforms_.classicLightGridScreen[1] = static_cast<float>(frame.lighting->classicLightOffset.deltaY);
			pendingPaletteShaderUniforms_.classicLightGridScreen[2] = static_cast<float>(frame.lighting->classicLightViewportHeight);
		}
		PopulatePaletteShaderSmoothLightSources(pendingPaletteShaderUniforms_, frame.lighting, composition.logicalSize);
		pendingMode_ = PendingMode::IndexedPalette;
		return true;
	}

	[[nodiscard]] bool PrepareOutputSurfaceFrame(const CompositionFrame &frame, SDL_Surface &outputSurface, RenderPerfCompositionStats &stats)
	{
		(void)stats;
		if (!available_ || outputSurface.pixels == nullptr || outputSurface.w <= 0 || outputSurface.h <= 0)
			return false;

		const bool mustLock = SDL_MUSTLOCK(&outputSurface);
		if (mustLock && !SDL_LockSurface(&outputSurface))
			return false;

		const int bytesPerPixel = SurfaceBytesPerPixel(outputSurface);
		outputRgba_.resize(static_cast<size_t>(outputSurface.w) * static_cast<size_t>(outputSurface.h) * 4);
		for (int y = 0; y < outputSurface.h; y++) {
			const uint8_t *src = static_cast<const uint8_t *>(outputSurface.pixels) + static_cast<ptrdiff_t>(y) * outputSurface.pitch;
			uint8_t *dst = outputRgba_.data() + static_cast<ptrdiff_t>(y) * outputSurface.w * 4;
			for (int x = 0; x < outputSurface.w; x++) {
				uint32_t pixel = 0;
				std::memcpy(&pixel, src + x * bytesPerPixel, bytesPerPixel);
				ReadSurfaceRgba(outputSurface, pixel, dst[x * 4 + 0], dst[x * 4 + 1], dst[x * 4 + 2], dst[x * 4 + 3]);
			}
		}

		if (mustLock)
			SDL_UnlockSurface(&outputSurface);

		pendingLogicalSize_ = frame.logicalSize.width > 0 && frame.logicalSize.height > 0 ? frame.logicalSize : Size { outputSurface.w, outputSurface.h };
		pendingOutputSize_ = { outputSurface.w, outputSurface.h };
		pendingLightShadowDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
		pendingSmoothLightingPresentation_ = false;
		pendingPaletteShaderUniforms_ = {};
		pendingMode_ = PendingMode::OutputSurface;
		return true;
	}

	void Present()
	{
		if (!available_ || pendingMode_ == PendingMode::None)
			return;
		if (pendingMode_ == PendingMode::IndexedPalette) {
			PresentIndexedFrame();
			return;
		}
		PresentOutputSurfaceFrame();
	}

private:
	enum class PendingMode : uint8_t {
		None,
		IndexedPalette,
		OutputSurface,
	};

	void PresentOutputSurfaceFrame()
	{
		if (pendingOutputSize_.width <= 0 || pendingOutputSize_.height <= 0)
			return;

		UpdateSwapchainParameters();
		if (!EnsureOutputTexture(pendingOutputSize_.width, pendingOutputSize_.height))
			return;
		if (!EnsureTransferBuffer(static_cast<uint32_t>(outputRgba_.size())))
			return;
		if (!CopyPendingOutputToTransferBuffer())
			return;

		SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device_);
		if (commandBuffer == nullptr) {
			Log("SDL_GPU palette compositor could not acquire command buffer: {}", SDL_GetError());
			return;
		}

		SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);
		if (copyPass == nullptr) {
			Log("SDL_GPU palette compositor could not begin copy pass: {}", SDL_GetError());
			(void)SDL_CancelGPUCommandBuffer(commandBuffer);
			return;
		}

		const SDL_GPUTextureTransferInfo source {
			transferBuffer_,
			0,
			static_cast<uint32_t>(pendingOutputSize_.width),
			static_cast<uint32_t>(pendingOutputSize_.height),
		};
		const SDL_GPUTextureRegion destination {
			outputTexture_,
			0,
			0,
			0,
			0,
			0,
			static_cast<uint32_t>(pendingOutputSize_.width),
			static_cast<uint32_t>(pendingOutputSize_.height),
			1,
		};
		SDL_UploadToGPUTexture(copyPass, &source, &destination, true);
		SDL_EndGPUCopyPass(copyPass);

		SDL_GPUTexture *swapchainTexture = nullptr;
		uint32_t swapchainWidth = 0;
		uint32_t swapchainHeight = 0;
		if (!SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window_, &swapchainTexture, &swapchainWidth, &swapchainHeight)) {
			Log("SDL_GPU palette compositor could not acquire swapchain texture: {}", SDL_GetError());
			(void)SDL_CancelGPUCommandBuffer(commandBuffer);
			return;
		}
		if (swapchainTexture != nullptr) {
			const Rectangle viewport = CalculateViewport({ static_cast<int>(swapchainWidth), static_cast<int>(swapchainHeight) }, pendingLogicalSize_);
			const SDL_GPUBlitInfo blit {
				{
				    outputTexture_,
				    0,
				    0,
				    0,
				    0,
				    static_cast<uint32_t>(pendingOutputSize_.width),
				    static_cast<uint32_t>(pendingOutputSize_.height),
				},
				{
				    swapchainTexture,
				    0,
				    0,
				    static_cast<uint32_t>(std::max(0, viewport.position.x)),
				    static_cast<uint32_t>(std::max(0, viewport.position.y)),
				    static_cast<uint32_t>(std::max(0, viewport.size.width)),
				    static_cast<uint32_t>(std::max(0, viewport.size.height)),
				},
				SDL_GPU_LOADOP_CLEAR,
				{ 0.F, 0.F, 0.F, 1.F },
				SDL_FLIP_NONE,
				*GetOptions().Graphics.scaleQuality == ScalingQuality::NearestPixel ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR,
				false,
				0,
				0,
				0,
			};
			SDL_BlitGPUTexture(commandBuffer, &blit);
		}

		if (!SDL_SubmitGPUCommandBuffer(commandBuffer)) {
			Log("SDL_GPU palette compositor could not submit command buffer: {}", SDL_GetError());
		}
	}

	void PresentIndexedFrame()
	{
		UpdateSwapchainParameters();
		if (!EnsurePaletteRenderResources())
			return;

		SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device_);
		if (commandBuffer == nullptr) {
			Log("SDL_GPU palette compositor could not acquire indexed presentation command buffer: {}", SDL_GetError());
			return;
		}

		SDL_GPUTexture *swapchainTexture = nullptr;
		uint32_t swapchainWidth = 0;
		uint32_t swapchainHeight = 0;
		if (!SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window_, &swapchainTexture, &swapchainWidth, &swapchainHeight)) {
			Log("SDL_GPU palette compositor could not acquire indexed presentation swapchain texture: {}", SDL_GetError());
			(void)SDL_CancelGPUCommandBuffer(commandBuffer);
			return;
		}
		if (swapchainTexture == nullptr) {
			(void)SDL_CancelGPUCommandBuffer(commandBuffer);
			return;
		}

		const Rectangle viewport = CalculateViewport({ static_cast<int>(swapchainWidth), static_cast<int>(swapchainHeight) }, pendingLogicalSize_);
		const Size presentationSize = pendingLogicalSize_;
		if (!EnsurePaletteExpandedTexture(presentationSize.width, presentationSize.height)) {
			(void)SDL_CancelGPUCommandBuffer(commandBuffer);
			return;
		}
		if (!UploadCursorOverlay(commandBuffer)) {
			Log("SDL_GPU palette compositor could not upload cursor overlay; skipping GPU cursor this frame");
			ClearCursorOverlay();
		}
		if (!UploadAutomapOverlay(commandBuffer, presentationSize)) {
			Log("SDL_GPU palette compositor could not upload automap overlay; skipping GPU automap this frame");
			ClearAutomapOverlay();
		}
		if (!UploadAutomapPlayerOverlay(commandBuffer, presentationSize)) {
			Log("SDL_GPU palette compositor could not upload automap player overlay; skipping GPU automap player overlay this frame");
			ClearAutomapPlayerOverlay();
		}

		const SDL_GPUColorTargetInfo colorTarget {
			paletteExpandedTexture_,
			0,
			0,
			{ 0.F, 0.F, 0.F, 1.F },
			SDL_GPU_LOADOP_CLEAR,
			SDL_GPU_STOREOP_STORE,
			nullptr,
			0,
			0,
			false,
			false,
			0,
			0,
		};
		SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTarget, 1, nullptr);
		if (renderPass == nullptr) {
			Log("SDL_GPU palette compositor could not begin palette expansion render pass: {}", SDL_GetError());
			(void)SDL_CancelGPUCommandBuffer(commandBuffer);
			return;
		}

		const SDL_GPUViewport gpuViewport {
			0.F,
			0.F,
			static_cast<float>(presentationSize.width),
			static_cast<float>(presentationSize.height),
			0.F,
			1.F,
		};
		const SDL_GPUTextureSamplerBinding samplers[] {
			{ indexTexture_, indexSampler_ },
			{ paletteTexture_, paletteSampler_ },
			{ lightTexture_, pendingPaletteShaderUniforms_.lightTextureIsDungeonGrid != 0 ? lightSampler_ : indexSampler_ },
			{ shadowTexture_, indexSampler_ },
		};
		SDL_SetGPUViewport(renderPass, &gpuViewport);
		SDL_BindGPUGraphicsPipeline(renderPass, palettePipeline_);
		SDL_BindGPUFragmentSamplers(renderPass, 0, samplers, 4);
		SDL_PushGPUFragmentUniformData(commandBuffer, 0, &pendingPaletteShaderUniforms_, sizeof(pendingPaletteShaderUniforms_));
		SDL_DrawGPUPrimitives(renderPass, 6, 1, 0, 0);
		SDL_EndGPURenderPass(renderPass);
		RenderAutomapOverlays(commandBuffer, presentationSize);
		RenderCursorOverlay(commandBuffer, presentationSize);

		const SDL_GPUBlitInfo blit {
			{
			    paletteExpandedTexture_,
			    0,
			    0,
			    0,
			    0,
			    static_cast<uint32_t>(presentationSize.width),
			    static_cast<uint32_t>(presentationSize.height),
			},
			{
			    swapchainTexture,
			    0,
			    0,
			    static_cast<uint32_t>(std::max(0, viewport.position.x)),
			    static_cast<uint32_t>(std::max(0, viewport.position.y)),
			    static_cast<uint32_t>(std::max(0, viewport.size.width)),
			    static_cast<uint32_t>(std::max(0, viewport.size.height)),
			},
			SDL_GPU_LOADOP_CLEAR,
			{ 0.F, 0.F, 0.F, 1.F },
			SDL_FLIP_NONE,
			*GetOptions().Graphics.scaleQuality == ScalingQuality::NearestPixel ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR,
			false,
			0,
			0,
			0,
		};
		SDL_BlitGPUTexture(commandBuffer, &blit);

		if (!SDL_SubmitGPUCommandBuffer(commandBuffer)) {
			Log("SDL_GPU palette compositor could not submit indexed presentation command buffer: {}", SDL_GetError());
		}
	}

	void ReleaseTexture()
	{
		if (outputTexture_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTexture(device_, outputTexture_);
		outputTexture_ = nullptr;
	}

	void ReleaseTransferBuffer()
	{
		if (transferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, transferBuffer_);
		transferBuffer_ = nullptr;
	}

	void ReleaseIndexedTextures()
	{
		if (indexTexture_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTexture(device_, indexTexture_);
		if (paletteTexture_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTexture(device_, paletteTexture_);
		indexTexture_ = nullptr;
		paletteTexture_ = nullptr;
		indexTextureUploaded_ = false;
		paletteTextureUploaded_ = false;
		uploadedIndexVersion_ = 0;
		uploadedPaletteVersion_ = 0;
	}

	void ReleaseLightingTextures()
	{
		if (lightTexture_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTexture(device_, lightTexture_);
		if (shadowTexture_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTexture(device_, shadowTexture_);
		lightTexture_ = nullptr;
		shadowTexture_ = nullptr;
		lightTextureUploaded_ = false;
		shadowTextureUploaded_ = false;
		uploadedLightVersion_ = 0;
		uploadedShadowVersion_ = 0;
		lightTextureWidth_ = 0;
		lightTextureHeight_ = 0;
		shadowTextureWidth_ = 0;
		shadowTextureHeight_ = 0;
		lightTextureFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
		shadowTextureFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
	}

	void ReleasePaletteExpandedTexture()
	{
		if (paletteExpandedTexture_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTexture(device_, paletteExpandedTexture_);
		paletteExpandedTexture_ = nullptr;
		paletteExpandedTextureWidth_ = 0;
		paletteExpandedTextureHeight_ = 0;
	}

	void ReleaseCursorOverlayTexture()
	{
		if (cursorOverlayTexture_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTexture(device_, cursorOverlayTexture_);
		cursorOverlayTexture_ = nullptr;
		cursorOverlayTextureWidth_ = 0;
		cursorOverlayTextureHeight_ = 0;
		cursorOverlayTextureUploaded_ = false;
		uploadedCursorOverlayVersion_ = 0;
	}

	void ReleaseIndexedTransferBuffers()
	{
		if (indexTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, indexTransferBuffer_);
		if (paletteTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, paletteTransferBuffer_);
		indexTransferBuffer_ = nullptr;
		paletteTransferBuffer_ = nullptr;
	}

	void ReleaseLightingTransferBuffers()
	{
		if (lightTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, lightTransferBuffer_);
		if (shadowTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, shadowTransferBuffer_);
		lightTransferBuffer_ = nullptr;
		shadowTransferBuffer_ = nullptr;
		lightTransferBufferSize_ = 0;
		shadowTransferBufferSize_ = 0;
	}

	void ReleaseCursorOverlayResources()
	{
		ReleaseCursorOverlayTexture();
		if (cursorOverlayTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, cursorOverlayTransferBuffer_);
		if (cursorOverlayPipeline_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUGraphicsPipeline(device_, cursorOverlayPipeline_);
		cursorOverlayTransferBuffer_ = nullptr;
		cursorOverlayPipeline_ = nullptr;
		cursorOverlayTransferBufferSize_ = 0;
		cursorOverlayPipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
		cursorOverlayPipelineFailed_ = false;
	}

	void ReleaseAutomapOverlayResources()
	{
		if (automapOverlayTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, automapOverlayTransferBuffer_);
		if (automapOverlayVertexBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUBuffer(device_, automapOverlayVertexBuffer_);
		if (automapPlayerOverlayTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, automapPlayerOverlayTransferBuffer_);
		if (automapPlayerOverlayVertexBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUBuffer(device_, automapPlayerOverlayVertexBuffer_);
		if (automapOverlayPipeline_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUGraphicsPipeline(device_, automapOverlayPipeline_);
		automapOverlayTransferBuffer_ = nullptr;
		automapOverlayVertexBuffer_ = nullptr;
		automapPlayerOverlayTransferBuffer_ = nullptr;
		automapPlayerOverlayVertexBuffer_ = nullptr;
		automapOverlayPipeline_ = nullptr;
		automapOverlayTransferBufferSize_ = 0;
		automapOverlayVertexBufferSize_ = 0;
		automapPlayerOverlayTransferBufferSize_ = 0;
		automapPlayerOverlayVertexBufferSize_ = 0;
		automapOverlayVertexBufferUploaded_ = false;
		automapPlayerOverlayVertexBufferUploaded_ = false;
		automapOverlayVertexCount_ = 0;
		automapPlayerOverlayVertexCount_ = 0;
		automapOverlayRejectRectCount_ = 0;
		automapOverlayPipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
		automapOverlayPipelineFailed_ = false;
		uploadedAutomapOverlayVersion_ = 0;
		uploadedAutomapOverlayPaletteVersion_ = 0;
		uploadedAutomapPlayerOverlayVersion_ = 0;
		uploadedAutomapPlayerOverlayPaletteVersion_ = 0;
	}

	void ReleasePalettePipelineResources()
	{
		if (palettePipeline_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUGraphicsPipeline(device_, palettePipeline_);
		if (indexSampler_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUSampler(device_, indexSampler_);
		if (paletteSampler_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUSampler(device_, paletteSampler_);
		if (lightSampler_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUSampler(device_, lightSampler_);
		palettePipeline_ = nullptr;
		indexSampler_ = nullptr;
		paletteSampler_ = nullptr;
		lightSampler_ = nullptr;
		palettePipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
		palettePipelineFailed_ = false;
	}

	[[nodiscard]] SDL_GPUShader *CreateShader(const PaletteShaderAsset &asset, const SDL_GPUShaderStage stage, const uint32_t samplerCount, const uint32_t uniformBufferCount = 0)
	{
		if (asset.format == SDL_GPU_SHADERFORMAT_INVALID || asset.code == nullptr || asset.codeSize == 0 || asset.entrypoint == nullptr)
			return nullptr;

		const SDL_GPUShaderCreateInfo createInfo {
			asset.codeSize,
			asset.code,
			asset.entrypoint,
			asset.format,
			stage,
			samplerCount,
			0,
			0,
			uniformBufferCount,
			0,
		};
		SDL_GPUShader *shader = SDL_CreateGPUShader(device_, &createInfo);
		if (shader == nullptr)
			Log("SDL_GPU palette compositor could not create {} shader: {}", ShaderFormatName(asset.format), SDL_GetError());
		return shader;
	}

	[[nodiscard]] bool EnsurePalettePipeline()
	{
		if (palettePipelineFailed_)
			return false;

		const SDL_GPUShaderFormat supportedFormats = SDL_GetGPUShaderFormats(device_);
		const PaletteShaderAsset vertexShaderAsset = SelectPaletteVertexShader(supportedFormats);
		const PaletteShaderAsset fragmentShaderAsset = SelectPaletteFragmentShader(supportedFormats);
		if (vertexShaderAsset.format == SDL_GPU_SHADERFORMAT_INVALID || fragmentShaderAsset.format == SDL_GPU_SHADERFORMAT_INVALID) {
			Log("SDL_GPU palette compositor has no shader asset compatible with this backend; falling back to CPU palette composition");
			palettePipelineFailed_ = true;
			return false;
		}

		if (palettePipeline_ != nullptr && palettePipelineFormat_ == PaletteExpandedTextureFormat)
			return true;

		if (palettePipeline_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUGraphicsPipeline(device_, palettePipeline_);
		palettePipeline_ = nullptr;
		palettePipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;

		SDL_GPUShader *vertexShader = CreateShader(vertexShaderAsset, SDL_GPU_SHADERSTAGE_VERTEX, 0);
		if (vertexShader == nullptr) {
			palettePipelineFailed_ = true;
			return false;
		}
		SDL_GPUShader *fragmentShader = CreateShader(fragmentShaderAsset, SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
		if (fragmentShader == nullptr) {
			SDL_ReleaseGPUShader(device_, vertexShader);
			palettePipelineFailed_ = true;
			return false;
		}

		const SDL_GPUColorTargetDescription colorTarget {
			PaletteExpandedTextureFormat,
			{},
		};
		SDL_GPUGraphicsPipelineCreateInfo createInfo {};
		createInfo.target_info.num_color_targets = 1;
		createInfo.target_info.color_target_descriptions = &colorTarget;
		createInfo.vertex_shader = vertexShader;
		createInfo.fragment_shader = fragmentShader;
		createInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		createInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
		createInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		createInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		createInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
		createInfo.rasterizer_state.enable_depth_clip = true;
		palettePipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &createInfo);
		SDL_ReleaseGPUShader(device_, vertexShader);
		SDL_ReleaseGPUShader(device_, fragmentShader);
		if (palettePipeline_ == nullptr) {
			Log("SDL_GPU palette compositor could not create palette graphics pipeline: {}", SDL_GetError());
			palettePipelineFailed_ = true;
			return false;
		}

		palettePipelineFormat_ = PaletteExpandedTextureFormat;
		return true;
	}

	[[nodiscard]] bool EnsureCursorOverlayPipeline()
	{
		if (cursorOverlayPipelineFailed_)
			return false;

		if (cursorOverlayPipeline_ != nullptr && cursorOverlayPipelineFormat_ == PaletteExpandedTextureFormat)
			return true;

		const SDL_GPUShaderFormat supportedFormats = SDL_GetGPUShaderFormats(device_);
		const PaletteShaderAsset vertexShaderAsset = SelectCursorOverlayVertexShader(supportedFormats);
		const PaletteShaderAsset fragmentShaderAsset = SelectCursorOverlayFragmentShader(supportedFormats);
		if (vertexShaderAsset.format == SDL_GPU_SHADERFORMAT_INVALID || fragmentShaderAsset.format == SDL_GPU_SHADERFORMAT_INVALID) {
			cursorOverlayPipelineFailed_ = true;
			return false;
		}

		if (cursorOverlayPipeline_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUGraphicsPipeline(device_, cursorOverlayPipeline_);
		cursorOverlayPipeline_ = nullptr;
		cursorOverlayPipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;

		SDL_GPUShader *vertexShader = CreateShader(vertexShaderAsset, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		if (vertexShader == nullptr) {
			cursorOverlayPipelineFailed_ = true;
			return false;
		}
		SDL_GPUShader *fragmentShader = CreateShader(fragmentShaderAsset, SDL_GPU_SHADERSTAGE_FRAGMENT, 1);
		if (fragmentShader == nullptr) {
			SDL_ReleaseGPUShader(device_, vertexShader);
			cursorOverlayPipelineFailed_ = true;
			return false;
		}

		const SDL_GPUColorTargetBlendState blendState {
			SDL_GPU_BLENDFACTOR_SRC_ALPHA,
			SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
			SDL_GPU_BLENDOP_ADD,
			SDL_GPU_BLENDFACTOR_ONE,
			SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
			SDL_GPU_BLENDOP_ADD,
			0,
			true,
			false,
			0,
			0,
		};
		const SDL_GPUColorTargetDescription colorTarget {
			PaletteExpandedTextureFormat,
			blendState,
		};
		SDL_GPUGraphicsPipelineCreateInfo createInfo {};
		createInfo.target_info.num_color_targets = 1;
		createInfo.target_info.color_target_descriptions = &colorTarget;
		createInfo.vertex_shader = vertexShader;
		createInfo.fragment_shader = fragmentShader;
		createInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		createInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
		createInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		createInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		createInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
		createInfo.rasterizer_state.enable_depth_clip = true;
		cursorOverlayPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &createInfo);
		SDL_ReleaseGPUShader(device_, vertexShader);
		SDL_ReleaseGPUShader(device_, fragmentShader);
		if (cursorOverlayPipeline_ == nullptr) {
			Log("SDL_GPU palette compositor could not create cursor overlay graphics pipeline: {}", SDL_GetError());
			cursorOverlayPipelineFailed_ = true;
			return false;
		}

		cursorOverlayPipelineFormat_ = PaletteExpandedTextureFormat;
		return true;
	}

	[[nodiscard]] bool EnsureAutomapOverlayPipeline()
	{
		if (automapOverlayPipelineFailed_)
			return false;

		if (automapOverlayPipeline_ != nullptr && automapOverlayPipelineFormat_ == PaletteExpandedTextureFormat)
			return true;

		const SDL_GPUShaderFormat supportedFormats = SDL_GetGPUShaderFormats(device_);
		const PaletteShaderAsset vertexShaderAsset = SelectAutomapOverlayVertexShader(supportedFormats);
		const PaletteShaderAsset fragmentShaderAsset = SelectAutomapOverlayFragmentShader(supportedFormats);
		if (vertexShaderAsset.format == SDL_GPU_SHADERFORMAT_INVALID || fragmentShaderAsset.format == SDL_GPU_SHADERFORMAT_INVALID) {
			automapOverlayPipelineFailed_ = true;
			return false;
		}

		if (automapOverlayPipeline_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUGraphicsPipeline(device_, automapOverlayPipeline_);
		automapOverlayPipeline_ = nullptr;
		automapOverlayPipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;

		SDL_GPUShader *vertexShader = CreateShader(vertexShaderAsset, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
		if (vertexShader == nullptr) {
			automapOverlayPipelineFailed_ = true;
			return false;
		}
		SDL_GPUShader *fragmentShader = CreateShader(fragmentShaderAsset, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
		if (fragmentShader == nullptr) {
			SDL_ReleaseGPUShader(device_, vertexShader);
			automapOverlayPipelineFailed_ = true;
			return false;
		}

		const SDL_GPUColorTargetBlendState blendState {
			SDL_GPU_BLENDFACTOR_SRC_ALPHA,
			SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
			SDL_GPU_BLENDOP_ADD,
			SDL_GPU_BLENDFACTOR_ONE,
			SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
			SDL_GPU_BLENDOP_ADD,
			0,
			true,
			false,
			0,
			0,
		};
		const SDL_GPUColorTargetDescription colorTarget {
			PaletteExpandedTextureFormat,
			blendState,
		};
		const SDL_GPUVertexBufferDescription vertexBufferDescription {
			0,
			sizeof(AutomapOverlayVertex),
			SDL_GPU_VERTEXINPUTRATE_VERTEX,
			0,
		};
		const SDL_GPUVertexAttribute vertexAttributes[] {
			{
			    0,
			    0,
			    SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
			    offsetof(AutomapOverlayVertex, x),
			},
			{
			    1,
			    0,
			    SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
			    offsetof(AutomapOverlayVertex, r),
			},
		};
		SDL_GPUGraphicsPipelineCreateInfo createInfo {};
		createInfo.target_info.num_color_targets = 1;
		createInfo.target_info.color_target_descriptions = &colorTarget;
		createInfo.vertex_input_state.vertex_buffer_descriptions = &vertexBufferDescription;
		createInfo.vertex_input_state.num_vertex_buffers = 1;
		createInfo.vertex_input_state.vertex_attributes = vertexAttributes;
		createInfo.vertex_input_state.num_vertex_attributes = 2;
		createInfo.vertex_shader = vertexShader;
		createInfo.fragment_shader = fragmentShader;
		createInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		createInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
		createInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
		createInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		createInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
		createInfo.rasterizer_state.enable_depth_clip = true;
		automapOverlayPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &createInfo);
		SDL_ReleaseGPUShader(device_, vertexShader);
		SDL_ReleaseGPUShader(device_, fragmentShader);
		if (automapOverlayPipeline_ == nullptr) {
			Log("SDL_GPU palette compositor could not create automap overlay graphics pipeline: {}", SDL_GetError());
			automapOverlayPipelineFailed_ = true;
			return false;
		}

		automapOverlayPipelineFormat_ = PaletteExpandedTextureFormat;
		return true;
	}

	[[nodiscard]] bool EnsureIndexedSamplers()
	{
		if (indexSampler_ == nullptr) {
			const SDL_GPUSamplerCreateInfo createInfo {
				SDL_GPU_FILTER_NEAREST,
				SDL_GPU_FILTER_NEAREST,
				SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
				SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				0.F,
				0.F,
				SDL_GPU_COMPAREOP_INVALID,
				0.F,
				0.F,
				false,
				false,
				0,
				0,
				0,
			};
			indexSampler_ = SDL_CreateGPUSampler(device_, &createInfo);
			if (indexSampler_ == nullptr) {
				Log("SDL_GPU palette compositor could not create index sampler: {}", SDL_GetError());
				return false;
			}
		}
		if (paletteSampler_ == nullptr) {
			const SDL_GPUSamplerCreateInfo createInfo {
				SDL_GPU_FILTER_NEAREST,
				SDL_GPU_FILTER_NEAREST,
				SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
				SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				0.F,
				0.F,
				SDL_GPU_COMPAREOP_INVALID,
				0.F,
				0.F,
				false,
				false,
				0,
				0,
				0,
			};
			paletteSampler_ = SDL_CreateGPUSampler(device_, &createInfo);
			if (paletteSampler_ == nullptr) {
				Log("SDL_GPU palette compositor could not create palette sampler: {}", SDL_GetError());
				return false;
			}
		}
		if (lightSampler_ == nullptr) {
			const SDL_GPUSamplerCreateInfo createInfo {
				SDL_GPU_FILTER_LINEAR,
				SDL_GPU_FILTER_LINEAR,
				SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
				SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
				0.F,
				0.F,
				SDL_GPU_COMPAREOP_INVALID,
				0.F,
				0.F,
				false,
				false,
				0,
				0,
				0,
			};
			lightSampler_ = SDL_CreateGPUSampler(device_, &createInfo);
			if (lightSampler_ == nullptr) {
				Log("SDL_GPU palette compositor could not create light sampler: {}", SDL_GetError());
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool EnsurePaletteRenderResources()
	{
		return EnsurePalettePipeline() && EnsureIndexedSamplers();
	}

	[[nodiscard]] bool EnsurePaletteExpandedTexture(const int width, const int height)
	{
		if (width <= 0 || height <= 0)
			return false;
		if (paletteExpandedTexture_ != nullptr && paletteExpandedTextureWidth_ == width && paletteExpandedTextureHeight_ == height)
			return true;

		if (!SDL_GPUTextureSupportsFormat(
		        device_,
		        PaletteExpandedTextureFormat,
		        SDL_GPU_TEXTURETYPE_2D,
		        SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)) {
			Log("SDL_GPU palette compositor cannot create RGBA color target for palette expansion; falling back to CPU palette composition");
			return false;
		}

		ReleasePaletteExpandedTexture();
		const SDL_GPUTextureCreateInfo createInfo {
			SDL_GPU_TEXTURETYPE_2D,
			PaletteExpandedTextureFormat,
			SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height),
			1,
			1,
			SDL_GPU_SAMPLECOUNT_1,
			0,
		};
		paletteExpandedTexture_ = SDL_CreateGPUTexture(device_, &createInfo);
		if (paletteExpandedTexture_ == nullptr) {
			Log("SDL_GPU palette compositor could not create palette-expanded texture: {}", SDL_GetError());
			return false;
		}
		paletteExpandedTextureWidth_ = width;
		paletteExpandedTextureHeight_ = height;
		return true;
	}

	[[nodiscard]] bool EnsureCursorOverlayTexture(const Size size)
	{
		if (size.width <= 0 || size.height <= 0)
			return false;
		if (cursorOverlayTexture_ != nullptr && cursorOverlayTextureWidth_ == size.width && cursorOverlayTextureHeight_ == size.height)
			return true;

		if (!SDL_GPUTextureSupportsFormat(device_, CursorOverlayTextureFormat, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
			Log("SDL_GPU palette compositor cannot create cursor overlay texture");
			return false;
		}

		ReleaseCursorOverlayTexture();
		const SDL_GPUTextureCreateInfo createInfo {
			SDL_GPU_TEXTURETYPE_2D,
			CursorOverlayTextureFormat,
			SDL_GPU_TEXTUREUSAGE_SAMPLER,
			static_cast<uint32_t>(size.width),
			static_cast<uint32_t>(size.height),
			1,
			1,
			SDL_GPU_SAMPLECOUNT_1,
			0,
		};
		cursorOverlayTexture_ = SDL_CreateGPUTexture(device_, &createInfo);
		if (cursorOverlayTexture_ == nullptr) {
			Log("SDL_GPU palette compositor could not create cursor overlay texture: {}", SDL_GetError());
			return false;
		}
		cursorOverlayTextureWidth_ = size.width;
		cursorOverlayTextureHeight_ = size.height;
		return true;
	}

	[[nodiscard]] bool EnsureIndexTexture(const int width, const int height)
	{
		if (indexTexture_ != nullptr && indexTextureWidth_ == width && indexTextureHeight_ == height)
			return true;

		if (indexTexture_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTexture(device_, indexTexture_);
		indexTexture_ = nullptr;
		indexTextureWidth_ = 0;
		indexTextureHeight_ = 0;
		indexTextureUploaded_ = false;
		uploadedIndexVersion_ = 0;

		const SDL_GPUTextureCreateInfo createInfo {
			SDL_GPU_TEXTURETYPE_2D,
			SDL_GPU_TEXTUREFORMAT_R8_UNORM,
			SDL_GPU_TEXTUREUSAGE_SAMPLER,
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height),
			1,
			1,
			SDL_GPU_SAMPLECOUNT_1,
			0,
		};
		indexTexture_ = SDL_CreateGPUTexture(device_, &createInfo);
		if (indexTexture_ == nullptr) {
			Log("SDL_GPU palette compositor could not create index texture: {}", SDL_GetError());
			return false;
		}
		indexTextureWidth_ = width;
		indexTextureHeight_ = height;
		return true;
	}

	[[nodiscard]] bool EnsurePaletteTexture()
	{
		if (paletteTexture_ != nullptr)
			return true;

		const SDL_GPUTextureCreateInfo createInfo {
			SDL_GPU_TEXTURETYPE_2D,
			SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			SDL_GPU_TEXTUREUSAGE_SAMPLER,
			static_cast<uint32_t>(PaletteTextureWidth),
			static_cast<uint32_t>(PaletteTextureHeight),
			1,
			1,
			SDL_GPU_SAMPLECOUNT_1,
			0,
		};
		paletteTexture_ = SDL_CreateGPUTexture(device_, &createInfo);
		if (paletteTexture_ == nullptr) {
			Log("SDL_GPU palette compositor could not create palette texture: {}", SDL_GetError());
			return false;
		}
		paletteTextureUploaded_ = false;
		uploadedPaletteVersion_ = 0;
		return true;
	}

	[[nodiscard]] bool EnsureLightingTexture(SDL_GPUTexture *&texture, int &currentWidth, int &currentHeight, SDL_GPUTextureFormat &currentFormat, bool &textureUploaded, uint64_t &uploadedVersion, const Size size, const SDL_GPUTextureFormat format, const char *name)
	{
		if (texture != nullptr && currentWidth == size.width && currentHeight == size.height && currentFormat == format)
			return true;

		if (!SDL_GPUTextureSupportsFormat(device_, format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
			Log("SDL_GPU palette compositor cannot create {} texture; falling back to CPU palette composition", name);
			return false;
		}

		if (texture != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTexture(device_, texture);
		texture = nullptr;
		currentWidth = 0;
		currentHeight = 0;
		currentFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
		textureUploaded = false;
		uploadedVersion = 0;

		const SDL_GPUTextureCreateInfo createInfo {
			SDL_GPU_TEXTURETYPE_2D,
			format,
			SDL_GPU_TEXTUREUSAGE_SAMPLER,
			static_cast<uint32_t>(size.width),
			static_cast<uint32_t>(size.height),
			1,
			1,
			SDL_GPU_SAMPLECOUNT_1,
			0,
		};
		texture = SDL_CreateGPUTexture(device_, &createInfo);
		if (texture == nullptr) {
			Log("SDL_GPU palette compositor could not create {} texture: {}", name, SDL_GetError());
			return false;
		}
		currentWidth = size.width;
		currentHeight = size.height;
		currentFormat = format;
		return true;
	}

	[[nodiscard]] bool EnsureLightingTextures(const CompositionLightingInputs &lightingInputs)
	{
		const SDL_GPUTextureFormat lightFormat = GpuTextureFormatForLightingBuffer(lightingInputs.light.format);
		const SDL_GPUTextureFormat shadowFormat = GpuTextureFormatForLightingBuffer(lightingInputs.shadow.format);
		return lightFormat != SDL_GPU_TEXTUREFORMAT_INVALID
		    && shadowFormat != SDL_GPU_TEXTUREFORMAT_INVALID
		    && EnsureLightingTexture(lightTexture_, lightTextureWidth_, lightTextureHeight_, lightTextureFormat_, lightTextureUploaded_, uploadedLightVersion_, lightingInputs.light.size, lightFormat, "light")
		    && EnsureLightingTexture(shadowTexture_, shadowTextureWidth_, shadowTextureHeight_, shadowTextureFormat_, shadowTextureUploaded_, uploadedShadowVersion_, lightingInputs.shadow.size, shadowFormat, "shadow");
	}

	[[nodiscard]] bool EnsureIndexedTransferBuffer(const uint32_t size)
	{
		if (indexTransferBuffer_ != nullptr && indexTransferBufferSize_ >= size)
			return true;

		if (indexTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, indexTransferBuffer_);
		indexTransferBuffer_ = nullptr;
		indexTransferBufferSize_ = 0;

		const SDL_GPUTransferBufferCreateInfo createInfo {
			SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			size,
			0,
		};
		indexTransferBuffer_ = SDL_CreateGPUTransferBuffer(device_, &createInfo);
		if (indexTransferBuffer_ == nullptr) {
			Log("SDL_GPU palette compositor could not create index transfer buffer: {}", SDL_GetError());
			return false;
		}
		indexTransferBufferSize_ = size;
		return true;
	}

	[[nodiscard]] bool EnsurePaletteTransferBuffer()
	{
		if (paletteTransferBuffer_ != nullptr)
			return true;

		const SDL_GPUTransferBufferCreateInfo createInfo {
			SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			PaletteUploadBytes,
			0,
		};
		paletteTransferBuffer_ = SDL_CreateGPUTransferBuffer(device_, &createInfo);
		if (paletteTransferBuffer_ == nullptr) {
			Log("SDL_GPU palette compositor could not create palette transfer buffer: {}", SDL_GetError());
			return false;
		}
		return true;
	}

	[[nodiscard]] bool EnsureLightingTransferBuffer(SDL_GPUTransferBuffer *&buffer, uint32_t &currentSize, const uint32_t size, const char *name)
	{
		if (buffer != nullptr && currentSize >= size)
			return true;

		if (buffer != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, buffer);
		buffer = nullptr;
		currentSize = 0;

		const SDL_GPUTransferBufferCreateInfo createInfo {
			SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			size,
			0,
		};
		buffer = SDL_CreateGPUTransferBuffer(device_, &createInfo);
		if (buffer == nullptr) {
			Log("SDL_GPU palette compositor could not create {} transfer buffer: {}", name, SDL_GetError());
			return false;
		}
		currentSize = size;
		return true;
	}

	[[nodiscard]] bool EnsureCursorOverlayTransferBuffer(const uint32_t size)
	{
		if (cursorOverlayTransferBuffer_ != nullptr && cursorOverlayTransferBufferSize_ >= size)
			return true;

		if (cursorOverlayTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, cursorOverlayTransferBuffer_);
		cursorOverlayTransferBuffer_ = nullptr;
		cursorOverlayTransferBufferSize_ = 0;

		const SDL_GPUTransferBufferCreateInfo createInfo {
			SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			size,
			0,
		};
		cursorOverlayTransferBuffer_ = SDL_CreateGPUTransferBuffer(device_, &createInfo);
		if (cursorOverlayTransferBuffer_ == nullptr) {
			Log("SDL_GPU palette compositor could not create cursor overlay transfer buffer: {}", SDL_GetError());
			return false;
		}
		cursorOverlayTransferBufferSize_ = size;
		return true;
	}

	[[nodiscard]] bool EnsureAutomapOverlayVertexBuffer(const uint32_t size)
	{
		if (automapOverlayVertexBuffer_ != nullptr && automapOverlayVertexBufferSize_ >= size)
			return true;

		if (automapOverlayVertexBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUBuffer(device_, automapOverlayVertexBuffer_);
		automapOverlayVertexBuffer_ = nullptr;
		automapOverlayVertexBufferSize_ = 0;
		automapOverlayVertexBufferUploaded_ = false;
		uploadedAutomapOverlayVersion_ = 0;
		uploadedAutomapOverlayPaletteVersion_ = 0;

		const SDL_GPUBufferCreateInfo createInfo {
			SDL_GPU_BUFFERUSAGE_VERTEX,
			size,
			0,
		};
		automapOverlayVertexBuffer_ = SDL_CreateGPUBuffer(device_, &createInfo);
		if (automapOverlayVertexBuffer_ == nullptr) {
			Log("SDL_GPU palette compositor could not create automap overlay vertex buffer: {}", SDL_GetError());
			return false;
		}
		automapOverlayVertexBufferSize_ = size;
		return true;
	}

	[[nodiscard]] bool EnsureAutomapOverlayTransferBuffer(const uint32_t size)
	{
		if (automapOverlayTransferBuffer_ != nullptr && automapOverlayTransferBufferSize_ >= size)
			return true;

		if (automapOverlayTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, automapOverlayTransferBuffer_);
		automapOverlayTransferBuffer_ = nullptr;
		automapOverlayTransferBufferSize_ = 0;

		const SDL_GPUTransferBufferCreateInfo createInfo {
			SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			size,
			0,
		};
		automapOverlayTransferBuffer_ = SDL_CreateGPUTransferBuffer(device_, &createInfo);
		if (automapOverlayTransferBuffer_ == nullptr) {
			Log("SDL_GPU palette compositor could not create automap overlay transfer buffer: {}", SDL_GetError());
			return false;
		}
		automapOverlayTransferBufferSize_ = size;
		return true;
	}

	[[nodiscard]] bool EnsureAutomapPlayerOverlayVertexBuffer(const uint32_t size)
	{
		if (automapPlayerOverlayVertexBuffer_ != nullptr && automapPlayerOverlayVertexBufferSize_ >= size)
			return true;

		if (automapPlayerOverlayVertexBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUBuffer(device_, automapPlayerOverlayVertexBuffer_);
		automapPlayerOverlayVertexBuffer_ = nullptr;
		automapPlayerOverlayVertexBufferSize_ = 0;
		automapPlayerOverlayVertexBufferUploaded_ = false;
		uploadedAutomapPlayerOverlayVersion_ = 0;
		uploadedAutomapPlayerOverlayPaletteVersion_ = 0;

		const SDL_GPUBufferCreateInfo createInfo {
			SDL_GPU_BUFFERUSAGE_VERTEX,
			size,
			0,
		};
		automapPlayerOverlayVertexBuffer_ = SDL_CreateGPUBuffer(device_, &createInfo);
		if (automapPlayerOverlayVertexBuffer_ == nullptr) {
			Log("SDL_GPU palette compositor could not create automap player overlay vertex buffer: {}", SDL_GetError());
			return false;
		}
		automapPlayerOverlayVertexBufferSize_ = size;
		return true;
	}

	[[nodiscard]] bool EnsureAutomapPlayerOverlayTransferBuffer(const uint32_t size)
	{
		if (automapPlayerOverlayTransferBuffer_ != nullptr && automapPlayerOverlayTransferBufferSize_ >= size)
			return true;

		if (automapPlayerOverlayTransferBuffer_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTransferBuffer(device_, automapPlayerOverlayTransferBuffer_);
		automapPlayerOverlayTransferBuffer_ = nullptr;
		automapPlayerOverlayTransferBufferSize_ = 0;

		const SDL_GPUTransferBufferCreateInfo createInfo {
			SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			size,
			0,
		};
		automapPlayerOverlayTransferBuffer_ = SDL_CreateGPUTransferBuffer(device_, &createInfo);
		if (automapPlayerOverlayTransferBuffer_ == nullptr) {
			Log("SDL_GPU palette compositor could not create automap player overlay transfer buffer: {}", SDL_GetError());
			return false;
		}
		automapPlayerOverlayTransferBufferSize_ = size;
		return true;
	}

	[[nodiscard]] bool CopyAttachmentToTransferBuffer(const CompositionAttachment &attachment, SDL_GPUTransferBuffer *transferBuffer, const CompositionAttachmentUploadPlan &plan, const char *name, RenderPerfCompositionStats &stats)
	{
		if (plan.action == CompositionAttachmentUploadAction::Skip)
			return true;

		const int bytesPerPixel = AttachmentBytesPerPixel(attachment.format);
		if (bytesPerPixel <= 0)
			return FailUpload(stats, CompositionUploadFallbackReason::InvalidFrame);

		void *mapped = SDL_MapGPUTransferBuffer(device_, transferBuffer, true);
		if (mapped == nullptr) {
			Log("SDL_GPU palette compositor could not map {} transfer buffer: {}", name, SDL_GetError());
			return FailUpload(stats, CompositionUploadFallbackReason::TransferMapFailed);
		}

		auto *dst = static_cast<uint8_t *>(mapped);
		for (const Rectangle &rect : plan.rects) {
			for (int y = 0; y < rect.size.height; y++) {
				const ptrdiff_t rowOffset = static_cast<ptrdiff_t>(rect.position.y + y) * attachment.pitch;
				const ptrdiff_t columnOffset = static_cast<ptrdiff_t>(rect.position.x) * bytesPerPixel;
				std::memcpy(dst + rowOffset + columnOffset, attachment.cpuPixels + rowOffset + columnOffset, static_cast<size_t>(rect.size.width) * bytesPerPixel);
			}
		}
		SDL_UnmapGPUTransferBuffer(device_, transferBuffer);
		return true;
	}

	void UploadAttachmentToTexture(SDL_GPUCopyPass *copyPass, SDL_GPUTransferBuffer *transferBuffer, SDL_GPUTexture *texture, const CompositionAttachment &attachment, const CompositionAttachmentUploadPlan &plan)
	{
		if (plan.action == CompositionAttachmentUploadAction::Skip)
			return;

		const int bytesPerPixel = AttachmentBytesPerPixel(attachment.format);
		const uint32_t pixelsPerRow = AttachmentTransferPixelsPerRow(attachment);
		const bool cycleTexture = plan.action == CompositionAttachmentUploadAction::Full;
		for (const Rectangle &rect : plan.rects) {
			const SDL_GPUTextureTransferInfo source {
				transferBuffer,
				static_cast<uint32_t>(static_cast<uint64_t>(rect.position.y) * static_cast<uint64_t>(attachment.pitch) + static_cast<uint64_t>(rect.position.x) * static_cast<uint64_t>(bytesPerPixel)),
				pixelsPerRow,
				static_cast<uint32_t>(attachment.logicalSize.height),
			};
			const SDL_GPUTextureRegion destination {
				texture,
				0,
				0,
				static_cast<uint32_t>(rect.position.x),
				static_cast<uint32_t>(rect.position.y),
				0,
				static_cast<uint32_t>(rect.size.width),
				static_cast<uint32_t>(rect.size.height),
				1,
			};
			SDL_UploadToGPUTexture(copyPass, &source, &destination, cycleTexture);
		}
	}

	[[nodiscard]] bool CopyCursorOverlayToTransferBuffer()
	{
		if (cursorOverlayPixels_.empty())
			return false;
		void *mapped = SDL_MapGPUTransferBuffer(device_, cursorOverlayTransferBuffer_, true);
		if (mapped == nullptr) {
			Log("SDL_GPU palette compositor could not map cursor overlay transfer buffer: {}", SDL_GetError());
			return false;
		}
		std::memcpy(mapped, cursorOverlayPixels_.data(), cursorOverlayPixels_.size());
		SDL_UnmapGPUTransferBuffer(device_, cursorOverlayTransferBuffer_);
		return true;
	}

	void AppendAutomapOverlayQuad(std::vector<AutomapOverlayVertex> &vertices, const Rectangle rect, const RgbColor color, const uint8_t alpha)
	{
		const float x0 = static_cast<float>(rect.position.x);
		const float y0 = static_cast<float>(rect.position.y);
		const float x1 = static_cast<float>(rect.position.x + rect.size.width);
		const float y1 = static_cast<float>(rect.position.y + rect.size.height);
		const float r = static_cast<float>(color.r) / 255.F;
		const float g = static_cast<float>(color.g) / 255.F;
		const float b = static_cast<float>(color.b) / 255.F;
		const float a = static_cast<float>(alpha) / 255.F;
		vertices.push_back({ x0, y0, r, g, b, a });
		vertices.push_back({ x1, y0, r, g, b, a });
		vertices.push_back({ x1, y1, r, g, b, a });
		vertices.push_back({ x0, y0, r, g, b, a });
		vertices.push_back({ x1, y1, r, g, b, a });
		vertices.push_back({ x0, y1, r, g, b, a });
	}

	[[nodiscard]] bool BuildAutomapOverlayVerticesFor(
	    const std::vector<RenderAutomapOverlayRect> &rects,
	    std::vector<AutomapOverlayVertex> &vertices,
	    uint32_t &vertexCount,
	    const Size presentationSize)
	{
		(void)presentationSize;
		vertices.clear();
		vertexCount = 0;
		if (rects.empty())
			return false;

		vertices.reserve(rects.size() * 6);
		for (const RenderAutomapOverlayRect &overlayRect : rects) {
			if (overlayRect.rect.size.width <= 0 || overlayRect.rect.size.height <= 0)
				continue;
			AppendAutomapOverlayQuad(vertices, overlayRect.rect, palettePixels_[overlayRect.colorIndex], overlayRect.alpha);
		}
		vertexCount = static_cast<uint32_t>(vertices.size());
		return vertexCount > 0;
	}

	[[nodiscard]] bool BuildAutomapOverlayVertices(const Size presentationSize)
	{
		if (!automapOverlayVisible_)
			return false;
		return BuildAutomapOverlayVerticesFor(automapOverlayRects_, automapOverlayVertices_, automapOverlayVertexCount_, presentationSize);
	}

	[[nodiscard]] bool BuildAutomapPlayerOverlayVertices(const Size presentationSize)
	{
		if (!automapPlayerOverlayVisible_)
			return false;
		return BuildAutomapOverlayVerticesFor(automapPlayerOverlayRects_, automapPlayerOverlayVertices_, automapPlayerOverlayVertexCount_, presentationSize);
	}

	[[nodiscard]] bool CopyAutomapOverlayToTransferBuffer(const std::vector<AutomapOverlayVertex> &vertices, SDL_GPUTransferBuffer *transferBuffer, const char *name)
	{
		if (vertices.empty())
			return false;
		void *mapped = SDL_MapGPUTransferBuffer(device_, transferBuffer, true);
		if (mapped == nullptr) {
			Log("SDL_GPU palette compositor could not map {} overlay transfer buffer: {}", name, SDL_GetError());
			return false;
		}
		std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(AutomapOverlayVertex));
		SDL_UnmapGPUTransferBuffer(device_, transferBuffer);
		return true;
	}

	[[nodiscard]] bool UploadCursorOverlay(SDL_GPUCommandBuffer *commandBuffer)
	{
		if (!cursorOverlayVisible_)
			return true;
		if (!EnsureCursorOverlayTexture(cursorOverlaySize_))
			return false;

		if (cursorOverlayTextureUploaded_ && uploadedCursorOverlayVersion_ == cursorOverlayVersion_)
			return true;

		const uint64_t uploadSize64 = static_cast<uint64_t>(cursorOverlaySize_.width) * static_cast<uint64_t>(cursorOverlaySize_.height) * 4;
		if (uploadSize64 == 0 || uploadSize64 > std::numeric_limits<uint32_t>::max())
			return false;
		const uint32_t uploadSize = static_cast<uint32_t>(uploadSize64);
		if (!EnsureCursorOverlayTransferBuffer(uploadSize) || !CopyCursorOverlayToTransferBuffer())
			return false;

		SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);
		if (copyPass == nullptr) {
			Log("SDL_GPU palette compositor could not begin cursor overlay copy pass: {}", SDL_GetError());
			return false;
		}

		const SDL_GPUTextureTransferInfo source {
			cursorOverlayTransferBuffer_,
			0,
			static_cast<uint32_t>(cursorOverlaySize_.width),
			static_cast<uint32_t>(cursorOverlaySize_.height),
		};
		const SDL_GPUTextureRegion destination {
			cursorOverlayTexture_,
			0,
			0,
			0,
			0,
			0,
			static_cast<uint32_t>(cursorOverlaySize_.width),
			static_cast<uint32_t>(cursorOverlaySize_.height),
			1,
		};
		SDL_UploadToGPUTexture(copyPass, &source, &destination, true);
		SDL_EndGPUCopyPass(copyPass);
		cursorOverlayTextureUploaded_ = true;
		uploadedCursorOverlayVersion_ = cursorOverlayVersion_;
		return true;
	}

	[[nodiscard]] bool UploadAutomapOverlay(SDL_GPUCommandBuffer *commandBuffer, const Size presentationSize)
	{
		if (!automapOverlayVisible_)
			return true;
		if (automapOverlayVertexBufferUploaded_
		    && uploadedAutomapOverlayVersion_ == automapOverlayVersion_
		    && uploadedAutomapOverlayPaletteVersion_ == uploadedPaletteVersion_) {
			AddRenderPerfAutomapOverlayStats(automapOverlayRects_.size(), automapOverlayVertexCount_, 0, false, true);
			return true;
		}
		if (!BuildAutomapOverlayVertices(presentationSize))
			return true;

		const uint64_t uploadSize64 = static_cast<uint64_t>(automapOverlayVertices_.size()) * sizeof(AutomapOverlayVertex);
		if (uploadSize64 == 0 || uploadSize64 > std::numeric_limits<uint32_t>::max())
			return false;
		const uint32_t uploadSize = static_cast<uint32_t>(uploadSize64);
		if (!EnsureAutomapOverlayVertexBuffer(uploadSize) || !EnsureAutomapOverlayTransferBuffer(uploadSize) || !CopyAutomapOverlayToTransferBuffer(automapOverlayVertices_, automapOverlayTransferBuffer_, "automap"))
			return false;

		SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);
		if (copyPass == nullptr) {
			Log("SDL_GPU palette compositor could not begin automap overlay copy pass: {}", SDL_GetError());
			return false;
		}

		const SDL_GPUTransferBufferLocation source {
			automapOverlayTransferBuffer_,
			0,
		};
		const SDL_GPUBufferRegion destination {
			automapOverlayVertexBuffer_,
			0,
			uploadSize,
		};
		SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
		SDL_EndGPUCopyPass(copyPass);
		automapOverlayVertexBufferUploaded_ = true;
		uploadedAutomapOverlayVersion_ = automapOverlayVersion_;
		uploadedAutomapOverlayPaletteVersion_ = uploadedPaletteVersion_;
		AddRenderPerfAutomapOverlayStats(automapOverlayRects_.size(), automapOverlayVertexCount_, uploadSize, true, false);
		return true;
	}

	[[nodiscard]] bool UploadAutomapPlayerOverlay(SDL_GPUCommandBuffer *commandBuffer, const Size presentationSize)
	{
		if (!automapPlayerOverlayVisible_)
			return true;
		if (automapPlayerOverlayVertexBufferUploaded_
		    && uploadedAutomapPlayerOverlayVersion_ == automapPlayerOverlayVersion_
		    && uploadedAutomapPlayerOverlayPaletteVersion_ == uploadedPaletteVersion_) {
			AddRenderPerfAutomapOverlayStats(automapPlayerOverlayRects_.size(), automapPlayerOverlayVertexCount_, 0, false, true);
			return true;
		}
		if (!BuildAutomapPlayerOverlayVertices(presentationSize))
			return true;

		const uint64_t uploadSize64 = static_cast<uint64_t>(automapPlayerOverlayVertices_.size()) * sizeof(AutomapOverlayVertex);
		if (uploadSize64 == 0 || uploadSize64 > std::numeric_limits<uint32_t>::max())
			return false;
		const uint32_t uploadSize = static_cast<uint32_t>(uploadSize64);
		if (!EnsureAutomapPlayerOverlayVertexBuffer(uploadSize) || !EnsureAutomapPlayerOverlayTransferBuffer(uploadSize) || !CopyAutomapOverlayToTransferBuffer(automapPlayerOverlayVertices_, automapPlayerOverlayTransferBuffer_, "automap player"))
			return false;

		SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);
		if (copyPass == nullptr) {
			Log("SDL_GPU palette compositor could not begin automap player overlay copy pass: {}", SDL_GetError());
			return false;
		}

		const SDL_GPUTransferBufferLocation source {
			automapPlayerOverlayTransferBuffer_,
			0,
		};
		const SDL_GPUBufferRegion destination {
			automapPlayerOverlayVertexBuffer_,
			0,
			uploadSize,
		};
		SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
		SDL_EndGPUCopyPass(copyPass);
		automapPlayerOverlayVertexBufferUploaded_ = true;
		uploadedAutomapPlayerOverlayVersion_ = automapPlayerOverlayVersion_;
		uploadedAutomapPlayerOverlayPaletteVersion_ = uploadedPaletteVersion_;
		AddRenderPerfAutomapOverlayStats(automapPlayerOverlayRects_.size(), automapPlayerOverlayVertexCount_, uploadSize, true, false);
		return true;
	}

	void DrawAutomapOverlayBuffer(
	    SDL_GPUCommandBuffer *commandBuffer,
	    SDL_GPURenderPass *renderPass,
	    const Size presentationSize,
	    SDL_GPUBuffer *vertexBuffer,
	    const uint32_t vertexCount,
	    const Displacement offset)
	{
		if (vertexBuffer == nullptr || vertexCount == 0)
			return;

		const SDL_GPUViewport gpuViewport {
			0.F,
			0.F,
			static_cast<float>(presentationSize.width),
			static_cast<float>(presentationSize.height),
			0.F,
			1.F,
		};
		const SDL_GPUBufferBinding vertexBufferBinding {
			vertexBuffer,
			0,
		};
		const AutomapOverlayShaderUniforms uniforms {
			{
			    static_cast<float>(std::max(1, presentationSize.width)),
			    static_cast<float>(std::max(1, presentationSize.height)),
			    0.F,
			    0.F,
			},
			{
			    static_cast<float>(offset.deltaX),
			    static_cast<float>(offset.deltaY),
			    0.F,
			    0.F,
			},
		};
		AutomapOverlayShaderUniforms clippedUniforms = uniforms;
		for (size_t i = 0; i < automapOverlayRejectRectCount_; i++) {
			const Rectangle &rect = automapOverlayRejectRects_[i];
			clippedUniforms.rejectRects[i] = {
				static_cast<float>(rect.position.x),
				static_cast<float>(rect.position.y),
				static_cast<float>(rect.position.x + rect.size.width),
				static_cast<float>(rect.position.y + rect.size.height),
			};
		}
		clippedUniforms.rejectRectCount[0] = static_cast<float>(automapOverlayRejectRectCount_);
		SDL_SetGPUViewport(renderPass, &gpuViewport);
		SDL_BindGPUGraphicsPipeline(renderPass, automapOverlayPipeline_);
		SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBufferBinding, 1);
		SDL_PushGPUVertexUniformData(commandBuffer, 0, &clippedUniforms, sizeof(clippedUniforms));
		SDL_PushGPUFragmentUniformData(commandBuffer, 0, &clippedUniforms, sizeof(clippedUniforms));
		SDL_DrawGPUPrimitives(renderPass, vertexCount, 1, 0, 0);
	}

	void RenderAutomapOverlays(SDL_GPUCommandBuffer *commandBuffer, const Size presentationSize)
	{
		if (!automapOverlayVisible_ && !automapPlayerOverlayVisible_)
			return;
		if (!EnsureAutomapOverlayPipeline())
			return;

		const SDL_GPUColorTargetInfo colorTarget {
			paletteExpandedTexture_,
			0,
			0,
			{ 0.F, 0.F, 0.F, 1.F },
			SDL_GPU_LOADOP_LOAD,
			SDL_GPU_STOREOP_STORE,
			nullptr,
			0,
			0,
			false,
			false,
			0,
			0,
		};
		SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTarget, 1, nullptr);
		if (renderPass == nullptr) {
			Log("SDL_GPU palette compositor could not begin automap overlay render pass: {}", SDL_GetError());
			return;
		}
		if (automapOverlayVisible_)
			DrawAutomapOverlayBuffer(commandBuffer, renderPass, presentationSize, automapOverlayVertexBuffer_, automapOverlayVertexCount_, automapOverlayOffset_);
		if (automapPlayerOverlayVisible_)
			DrawAutomapOverlayBuffer(commandBuffer, renderPass, presentationSize, automapPlayerOverlayVertexBuffer_, automapPlayerOverlayVertexCount_, {});
		SDL_EndGPURenderPass(renderPass);
	}

	void RenderCursorOverlay(SDL_GPUCommandBuffer *commandBuffer, const Size presentationSize)
	{
		if (!cursorOverlayVisible_ || cursorOverlayTexture_ == nullptr || cursorOverlaySize_.width <= 0 || cursorOverlaySize_.height <= 0)
			return;
		if (!EnsureCursorOverlayPipeline() || indexSampler_ == nullptr)
			return;

		const SDL_GPUColorTargetInfo colorTarget {
			paletteExpandedTexture_,
			0,
			0,
			{ 0.F, 0.F, 0.F, 1.F },
			SDL_GPU_LOADOP_LOAD,
			SDL_GPU_STOREOP_STORE,
			nullptr,
			0,
			0,
			false,
			false,
			0,
			0,
		};
		SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTarget, 1, nullptr);
		if (renderPass == nullptr) {
			Log("SDL_GPU palette compositor could not begin cursor overlay render pass: {}", SDL_GetError());
			return;
		}

		const SDL_GPUViewport gpuViewport {
			0.F,
			0.F,
			static_cast<float>(presentationSize.width),
			static_cast<float>(presentationSize.height),
			0.F,
			1.F,
		};
		const SDL_GPUTextureSamplerBinding sampler { cursorOverlayTexture_, indexSampler_ };
		const CursorOverlayShaderUniforms uniforms {
			{
			    static_cast<float>(std::max(1, presentationSize.width)),
			    static_cast<float>(std::max(1, presentationSize.height)),
			    0.F,
			    0.F,
			},
			{
			    static_cast<float>(cursorOverlayPosition_.x),
			    static_cast<float>(cursorOverlayPosition_.y),
			    static_cast<float>(cursorOverlaySize_.width),
			    static_cast<float>(cursorOverlaySize_.height),
			},
		};
		SDL_SetGPUViewport(renderPass, &gpuViewport);
		SDL_BindGPUGraphicsPipeline(renderPass, cursorOverlayPipeline_);
		SDL_BindGPUFragmentSamplers(renderPass, 0, &sampler, 1);
		SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
		SDL_DrawGPUPrimitives(renderPass, 6, 1, 0, 0);
		SDL_EndGPURenderPass(renderPass);
	}

	[[nodiscard]] bool LightingInputsAreUsable(const CompositionLightingInputs *lightingInputs, const Size logicalSize) const
	{
		if (lightingInputs == nullptr || !lightingInputs->light.IsValid() || !lightingInputs->shadow.IsValid())
			return false;
		if (lightingInputs->shadow.size != logicalSize)
			return false;
		if ((lightingInputs->light.format != CompositionLightingBufferFormat::Rgba8 && lightingInputs->light.format != CompositionLightingBufferFormat::Alpha8)
		    || lightingInputs->shadow.format != CompositionLightingBufferFormat::Alpha8)
			return false;
		if (lightingInputs->lightStoresDungeonGrid) {
			if (lightingInputs->light.format != CompositionLightingBufferFormat::Alpha8
			    || lightingInputs->light.size.width <= 0
			    || lightingInputs->light.size.height <= 0
			    || lightingInputs->light.pitch < lightingInputs->light.size.width
			    || lightingInputs->classicLightViewportHeight <= 0) {
				return false;
			}
		} else if (lightingInputs->light.size != logicalSize) {
			return false;
		} else if (lightingInputs->light.format == CompositionLightingBufferFormat::Rgba8) {
			if (lightingInputs->light.pitch < logicalSize.width * 4 || lightingInputs->light.pitch % 4 != 0)
				return false;
		} else if (lightingInputs->light.pitch < logicalSize.width) {
			return false;
		}
		if (lightingInputs->shadow.pitch < logicalSize.width)
			return false;
		return true;
	}

	[[nodiscard]] CompositionAttachment PreparePaletteAttachment(const PaletteSnapshot &palette)
	{
		for (size_t paletteIndex = 0; paletteIndex < PaletteTextureWidth; paletteIndex++) {
			palettePixels_[paletteIndex] = palette.colors[paletteIndex];
		}
		for (size_t lightLevel = 0; lightLevel < NumLightingLevels; lightLevel++) {
			RgbColor *row = palettePixels_.data() + (PaletteClassicLightRowOffset + lightLevel) * PaletteTextureWidth;
			const auto &lightTable = LightTables[lightLevel];
			for (size_t paletteIndex = 0; paletteIndex < PaletteTextureWidth; paletteIndex++) {
				row[paletteIndex] = palette.colors[lightTable[paletteIndex]];
			}
		}
		return {
			CompositionAttachmentRole::Palette,
			CompositionAttachmentFormat::PaletteRgba8,
			{ PaletteTextureWidth, PaletteTextureHeight },
			PaletteTextureWidth * 4,
			palette.version,
			{},
			reinterpret_cast<const uint8_t *>(palettePixels_.data()),
		};
	}

	[[nodiscard]] bool UploadIndexedInputs(const AcceleratedPaletteFrame &frame, RenderPerfCompositionStats &stats)
	{
		const CompositionFrame &composition = frame.composition;
		if (!available_ || composition.indexBuffer.pixels == nullptr || composition.logicalSize.width <= 0 || composition.logicalSize.height <= 0)
			return FailUpload(stats, CompositionUploadFallbackReason::InvalidFrame);
		if (composition.indexBuffer.width < composition.logicalSize.width || composition.indexBuffer.height < composition.logicalSize.height || composition.indexBuffer.pitch < composition.logicalSize.width)
			return FailUpload(stats, CompositionUploadFallbackReason::InvalidFrame);
		if (!LightingInputsAreUsable(frame.lighting, composition.logicalSize))
			return FailUpload(stats, CompositionUploadFallbackReason::InvalidLightingInputs);

		const CompositionAttachment *indexedAttachmentPtr = FindCompositionAttachment(composition.attachments, CompositionAttachmentRole::IndexedAlbedo);
		const CompositionAttachment *paletteAttachmentPtr = FindCompositionAttachment(composition.attachments, CompositionAttachmentRole::Palette);
		CompositionAttachment indexedAttachment = indexedAttachmentPtr != nullptr
		    ? *indexedAttachmentPtr
		    : MakeIndexedAlbedoAttachment(composition.indexBuffer, composition.logicalSize, composition.dirtyRects);
		(void)paletteAttachmentPtr;
		CompositionAttachment paletteAttachment = PreparePaletteAttachment(composition.palette);
		CompositionAttachment lightAttachment = MakeLightingAttachment(CompositionAttachmentRole::LightAccumulation, frame.lighting->light);
		CompositionAttachment shadowAttachment = MakeLightingAttachment(CompositionAttachmentRole::ShadowMask, frame.lighting->shadow);

		const uint64_t indexUploadSize64 = AttachmentTransferBufferSize(indexedAttachment);
		const uint64_t paletteUploadSize64 = AttachmentTransferBufferSize(paletteAttachment);
		const uint64_t lightUploadSize64 = AttachmentTransferBufferSize(lightAttachment);
		const uint64_t shadowUploadSize64 = AttachmentTransferBufferSize(shadowAttachment);
		if (indexUploadSize64 > std::numeric_limits<uint32_t>::max()
		    || paletteUploadSize64 > std::numeric_limits<uint32_t>::max()
		    || lightUploadSize64 > std::numeric_limits<uint32_t>::max()
		    || shadowUploadSize64 > std::numeric_limits<uint32_t>::max()) {
			return FailUpload(stats, CompositionUploadFallbackReason::UploadSizeTooLarge);
		}
		const uint32_t indexUploadSize = static_cast<uint32_t>(indexUploadSize64);
		const uint32_t lightUploadSize = static_cast<uint32_t>(lightUploadSize64);
		const uint32_t shadowUploadSize = static_cast<uint32_t>(shadowUploadSize64);

		if (!EnsureIndexTexture(composition.logicalSize.width, composition.logicalSize.height))
			return FailUpload(stats, CompositionUploadFallbackReason::ResourceUnavailable);
		if (!EnsurePaletteTexture())
			return FailUpload(stats, CompositionUploadFallbackReason::ResourceUnavailable);
		if (!EnsureLightingTextures(*frame.lighting))
			return FailUpload(stats, CompositionUploadFallbackReason::ResourceUnavailable);

		const CompositionAttachmentUploadPlan indexPlan = PlanCompositionAttachmentUpload(indexedAttachment, indexTextureUploaded_, uploadedIndexVersion_);
		const CompositionAttachmentUploadPlan palettePlan = PlanCompositionAttachmentUpload(paletteAttachment, paletteTextureUploaded_, uploadedPaletteVersion_);
		const CompositionAttachmentUploadPlan lightPlan = PlanCompositionAttachmentUpload(lightAttachment, lightTextureUploaded_, uploadedLightVersion_);
		const CompositionAttachmentUploadPlan shadowPlan = PlanCompositionAttachmentUpload(shadowAttachment, shadowTextureUploaded_, uploadedShadowVersion_);
		const bool hasUpload = indexPlan.action != CompositionAttachmentUploadAction::Skip
		    || palettePlan.action != CompositionAttachmentUploadAction::Skip
		    || lightPlan.action != CompositionAttachmentUploadAction::Skip
		    || shadowPlan.action != CompositionAttachmentUploadAction::Skip;
		if (!hasUpload) {
			RecordSuccessfulUpload(stats, indexPlan);
			RecordSuccessfulUpload(stats, palettePlan);
			RecordSuccessfulUpload(stats, lightPlan);
			RecordSuccessfulUpload(stats, shadowPlan);
			return true;
		}

		if (indexPlan.action != CompositionAttachmentUploadAction::Skip) {
			if (!EnsureIndexedTransferBuffer(indexUploadSize))
				return FailUpload(stats, CompositionUploadFallbackReason::ResourceUnavailable);
			if (!CopyAttachmentToTransferBuffer(indexedAttachment, indexTransferBuffer_, indexPlan, "index", stats))
				return false;
		}
		if (palettePlan.action != CompositionAttachmentUploadAction::Skip) {
			if (!EnsurePaletteTransferBuffer())
				return FailUpload(stats, CompositionUploadFallbackReason::ResourceUnavailable);
			if (!CopyAttachmentToTransferBuffer(paletteAttachment, paletteTransferBuffer_, palettePlan, "palette", stats))
				return false;
		}
		if (lightPlan.action != CompositionAttachmentUploadAction::Skip) {
			if (!EnsureLightingTransferBuffer(lightTransferBuffer_, lightTransferBufferSize_, lightUploadSize, "light"))
				return FailUpload(stats, CompositionUploadFallbackReason::ResourceUnavailable);
			if (!CopyAttachmentToTransferBuffer(lightAttachment, lightTransferBuffer_, lightPlan, "light", stats))
				return false;
		}
		if (shadowPlan.action != CompositionAttachmentUploadAction::Skip) {
			if (!EnsureLightingTransferBuffer(shadowTransferBuffer_, shadowTransferBufferSize_, shadowUploadSize, "shadow"))
				return FailUpload(stats, CompositionUploadFallbackReason::ResourceUnavailable);
			if (!CopyAttachmentToTransferBuffer(shadowAttachment, shadowTransferBuffer_, shadowPlan, "shadow", stats))
				return false;
		}

		SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device_);
		if (commandBuffer == nullptr) {
			Log("SDL_GPU palette compositor could not acquire indexed upload command buffer: {}", SDL_GetError());
			return FailUpload(stats, CompositionUploadFallbackReason::CommandBufferUnavailable);
		}

		SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);
		if (copyPass == nullptr) {
			Log("SDL_GPU palette compositor could not begin indexed upload copy pass: {}", SDL_GetError());
			(void)SDL_CancelGPUCommandBuffer(commandBuffer);
			return FailUpload(stats, CompositionUploadFallbackReason::CopyPassUnavailable);
		}

		UploadAttachmentToTexture(copyPass, indexTransferBuffer_, indexTexture_, indexedAttachment, indexPlan);
		UploadAttachmentToTexture(copyPass, paletteTransferBuffer_, paletteTexture_, paletteAttachment, palettePlan);
		UploadAttachmentToTexture(copyPass, lightTransferBuffer_, lightTexture_, lightAttachment, lightPlan);
		UploadAttachmentToTexture(copyPass, shadowTransferBuffer_, shadowTexture_, shadowAttachment, shadowPlan);
		SDL_EndGPUCopyPass(copyPass);

		if (!SDL_SubmitGPUCommandBuffer(commandBuffer)) {
			Log("SDL_GPU palette compositor could not submit indexed upload command buffer: {}", SDL_GetError());
			return FailUpload(stats, CompositionUploadFallbackReason::SubmitFailed);
		}

		if (indexPlan.action != CompositionAttachmentUploadAction::Skip) {
			indexTextureUploaded_ = true;
			uploadedIndexVersion_ = indexedAttachment.version;
		}
		if (palettePlan.action != CompositionAttachmentUploadAction::Skip) {
			paletteTextureUploaded_ = true;
			uploadedPaletteVersion_ = paletteAttachment.version;
		}
		if (lightPlan.action != CompositionAttachmentUploadAction::Skip) {
			lightTextureUploaded_ = true;
			uploadedLightVersion_ = lightAttachment.version;
		}
		if (shadowPlan.action != CompositionAttachmentUploadAction::Skip) {
			shadowTextureUploaded_ = true;
			uploadedShadowVersion_ = shadowAttachment.version;
		}
		RecordSuccessfulUpload(stats, indexPlan);
		RecordSuccessfulUpload(stats, palettePlan);
		RecordSuccessfulUpload(stats, lightPlan);
		RecordSuccessfulUpload(stats, shadowPlan);
		return true;
	}

	[[nodiscard]] bool EnsureOutputTexture(const int width, const int height)
	{
		if (outputTexture_ != nullptr && outputTextureWidth_ == width && outputTextureHeight_ == height)
			return true;

		ReleaseTexture();
		const SDL_GPUTextureCreateInfo createInfo {
			SDL_GPU_TEXTURETYPE_2D,
			SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			SDL_GPU_TEXTUREUSAGE_SAMPLER,
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height),
			1,
			1,
			SDL_GPU_SAMPLECOUNT_1,
			0,
		};
		outputTexture_ = SDL_CreateGPUTexture(device_, &createInfo);
		if (outputTexture_ == nullptr) {
			Log("SDL_GPU palette compositor could not create output texture: {}", SDL_GetError());
			return false;
		}
		outputTextureWidth_ = width;
		outputTextureHeight_ = height;
		return true;
	}

	[[nodiscard]] bool EnsureTransferBuffer(const uint32_t size)
	{
		if (transferBuffer_ != nullptr && transferBufferSize_ >= size)
			return true;

		ReleaseTransferBuffer();
		const SDL_GPUTransferBufferCreateInfo createInfo {
			SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
			size,
			0,
		};
		transferBuffer_ = SDL_CreateGPUTransferBuffer(device_, &createInfo);
		if (transferBuffer_ == nullptr) {
			Log("SDL_GPU palette compositor could not create transfer buffer: {}", SDL_GetError());
			return false;
		}
		transferBufferSize_ = size;
		return true;
	}

	[[nodiscard]] bool CopyPendingOutputToTransferBuffer()
	{
		void *mapped = SDL_MapGPUTransferBuffer(device_, transferBuffer_, true);
		if (mapped == nullptr) {
			Log("SDL_GPU palette compositor could not map transfer buffer: {}", SDL_GetError());
			return false;
		}
		std::memcpy(mapped, outputRgba_.data(), outputRgba_.size());
		SDL_UnmapGPUTransferBuffer(device_, transferBuffer_);
		return true;
	}

	void UpdateSwapchainParameters()
	{
		const SDL_GPUPresentMode presentMode = ChoosePresentMode();
		if (presentModeInitialized_ && lastPresentMode_ == presentMode)
			return;
		if (!SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, presentMode)) {
			Log("SDL_GPU palette compositor could not set swapchain parameters: {}", SDL_GetError());
			return;
		}
		lastPresentMode_ = presentMode;
		presentModeInitialized_ = true;
	}

	[[nodiscard]] SDL_GPUPresentMode ChoosePresentMode() const
	{
		if (*GetOptions().Graphics.frameRateControl == FrameRateControl::VerticalSync)
			return SDL_GPU_PRESENTMODE_VSYNC;
		if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE))
			return SDL_GPU_PRESENTMODE_IMMEDIATE;
		if (SDL_WindowSupportsGPUPresentMode(device_, window_, SDL_GPU_PRESENTMODE_MAILBOX))
			return SDL_GPU_PRESENTMODE_MAILBOX;
		return SDL_GPU_PRESENTMODE_VSYNC;
	}

	[[nodiscard]] Rectangle CalculateViewport(const Size drawableSize, const Size logicalSize) const
	{
		if (drawableSize.width <= 0 || drawableSize.height <= 0 || logicalSize.width <= 0 || logicalSize.height <= 0)
			return { { 0, 0 }, drawableSize };

		if (*GetOptions().Graphics.integerScaling) {
			const int scale = std::max(1, std::min(drawableSize.width / logicalSize.width, drawableSize.height / logicalSize.height));
			const Size scaledSize { logicalSize.width * scale, logicalSize.height * scale };
			return {
				{ (drawableSize.width - scaledSize.width) / 2, (drawableSize.height - scaledSize.height) / 2 },
				scaledSize,
			};
		}

		const double drawableAspect = static_cast<double>(drawableSize.width) / drawableSize.height;
		const double logicalAspect = static_cast<double>(logicalSize.width) / logicalSize.height;
		Size scaledSize {};
		if (drawableAspect > logicalAspect) {
			scaledSize.height = drawableSize.height;
			scaledSize.width = static_cast<int>(drawableSize.height * logicalAspect);
		} else {
			scaledSize.width = drawableSize.width;
			scaledSize.height = static_cast<int>(drawableSize.width / logicalAspect);
		}
		return {
			{ (drawableSize.width - scaledSize.width) / 2, (drawableSize.height - scaledSize.height) / 2 },
			scaledSize,
		};
	}

	SDL_GPUDevice *device_ = nullptr;
	SDL_Window *window_ = nullptr;
	SDL_GPUTexture *outputTexture_ = nullptr;
	SDL_GPUTexture *indexTexture_ = nullptr;
	SDL_GPUTexture *paletteTexture_ = nullptr;
	SDL_GPUTexture *paletteExpandedTexture_ = nullptr;
	SDL_GPUTexture *lightTexture_ = nullptr;
	SDL_GPUTexture *shadowTexture_ = nullptr;
	SDL_GPUTexture *cursorOverlayTexture_ = nullptr;
	SDL_GPUGraphicsPipeline *palettePipeline_ = nullptr;
	SDL_GPUGraphicsPipeline *cursorOverlayPipeline_ = nullptr;
	SDL_GPUGraphicsPipeline *automapOverlayPipeline_ = nullptr;
	SDL_GPUSampler *indexSampler_ = nullptr;
	SDL_GPUSampler *paletteSampler_ = nullptr;
	SDL_GPUSampler *lightSampler_ = nullptr;
	SDL_GPUBuffer *automapOverlayVertexBuffer_ = nullptr;
	SDL_GPUBuffer *automapPlayerOverlayVertexBuffer_ = nullptr;
	SDL_GPUTransferBuffer *transferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *indexTransferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *paletteTransferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *lightTransferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *shadowTransferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *cursorOverlayTransferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *automapOverlayTransferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *automapPlayerOverlayTransferBuffer_ = nullptr;
	bool windowClaimed_ = false;
	bool available_ = false;
	bool presentModeInitialized_ = false;
	bool loggedIndexedDeferral_ = false;
	bool loggedIndexedPresentation_ = false;
	bool loggedLightingInputs_ = false;
	bool loggedCursorOverlay_ = false;
	bool loggedAutomapOverlay_ = false;
	RenderLightShadowDiagnosticMode loggedDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
	bool palettePipelineFailed_ = false;
	bool cursorOverlayPipelineFailed_ = false;
	bool automapOverlayPipelineFailed_ = false;
	bool indexTextureUploaded_ = false;
	bool paletteTextureUploaded_ = false;
	bool lightTextureUploaded_ = false;
	bool shadowTextureUploaded_ = false;
	bool cursorOverlayTextureUploaded_ = false;
	bool cursorOverlayVisible_ = false;
	bool automapOverlayVertexBufferUploaded_ = false;
	bool automapOverlayVisible_ = false;
	bool automapPlayerOverlayVertexBufferUploaded_ = false;
	bool automapPlayerOverlayVisible_ = false;
	int outputTextureWidth_ = 0;
	int outputTextureHeight_ = 0;
	int indexTextureWidth_ = 0;
	int indexTextureHeight_ = 0;
	int paletteExpandedTextureWidth_ = 0;
	int paletteExpandedTextureHeight_ = 0;
	int lightTextureWidth_ = 0;
	int lightTextureHeight_ = 0;
	int shadowTextureWidth_ = 0;
	int shadowTextureHeight_ = 0;
	int cursorOverlayTextureWidth_ = 0;
	int cursorOverlayTextureHeight_ = 0;
	SDL_GPUTextureFormat lightTextureFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
	SDL_GPUTextureFormat shadowTextureFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
	uint32_t transferBufferSize_ = 0;
	uint32_t indexTransferBufferSize_ = 0;
	uint32_t lightTransferBufferSize_ = 0;
	uint32_t shadowTransferBufferSize_ = 0;
	uint32_t cursorOverlayTransferBufferSize_ = 0;
	uint32_t automapOverlayVertexBufferSize_ = 0;
	uint32_t automapOverlayTransferBufferSize_ = 0;
	uint32_t automapOverlayVertexCount_ = 0;
	uint32_t automapPlayerOverlayVertexBufferSize_ = 0;
	uint32_t automapPlayerOverlayTransferBufferSize_ = 0;
	uint32_t automapPlayerOverlayVertexCount_ = 0;
	uint64_t uploadedIndexVersion_ = 0;
	uint64_t uploadedPaletteVersion_ = 0;
	uint64_t uploadedLightVersion_ = 0;
	uint64_t uploadedShadowVersion_ = 0;
	uint64_t uploadedCursorOverlayVersion_ = 0;
	uint64_t uploadedAutomapOverlayVersion_ = 0;
	uint64_t uploadedAutomapOverlayPaletteVersion_ = 0;
	uint64_t uploadedAutomapPlayerOverlayVersion_ = 0;
	uint64_t uploadedAutomapPlayerOverlayPaletteVersion_ = 0;
	uint64_t cursorOverlayVersion_ = 0;
	uint64_t automapOverlayVersion_ = 0;
	uint64_t automapPlayerOverlayVersion_ = 0;
	uint64_t automapOverlayInputVersion_ = 0;
	uint64_t automapPlayerOverlayInputVersion_ = 0;
	SDL_GPUPresentMode lastPresentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
	SDL_GPUTextureFormat palettePipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
	SDL_GPUTextureFormat cursorOverlayPipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
	SDL_GPUTextureFormat automapOverlayPipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
	PendingMode pendingMode_ = PendingMode::None;
	RenderLightShadowDiagnosticMode pendingLightShadowDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
	bool pendingSmoothLightingPresentation_ = false;
	PaletteShaderUniforms pendingPaletteShaderUniforms_ {};
	Point cursorOverlayPosition_ {};
	Displacement automapOverlayOffset_ {};
	Size cursorOverlaySize_ {};
	Size pendingLogicalSize_ {};
	Size pendingOutputSize_ {};
	std::array<Rectangle, AutomapOverlayMaxRejectRects> automapOverlayRejectRects_ {};
	size_t automapOverlayRejectRectCount_ = 0;
	std::array<RgbColor, PaletteTextureWidth * PaletteTextureHeight> palettePixels_ {};
	std::vector<uint8_t> outputRgba_;
	std::vector<uint8_t> cursorOverlayPixels_;
	std::vector<RenderAutomapOverlayRect> automapOverlayRects_;
	std::vector<RenderAutomapOverlayRect> automapPlayerOverlayRects_;
	std::vector<AutomapOverlayVertex> automapOverlayVertices_;
	std::vector<AutomapOverlayVertex> automapPlayerOverlayVertices_;
};

SdlGpuPaletteCompositorState &SdlGpuState()
{
	static SdlGpuPaletteCompositorState state;
	return state;
}

class SdlGpuPalettePresenter final : public IAcceleratedPalettePresenter {
public:
	std::string_view Name() const override
	{
		return SdlGpuBackendName;
	}

	bool IsAvailable() const override
	{
		return SdlGpuState().IsAvailable();
	}

	bool PrepareIndexedFrame(const AcceleratedPaletteFrame &frame, RenderPerfCompositionStats &stats) override
	{
		return SdlGpuState().PrepareIndexedFrame(frame, stats);
	}

	bool PrepareOutputSurfaceFrame(const AcceleratedPaletteFrame &frame, SDL_Surface &outputSurface, RenderPerfCompositionStats &stats) override
	{
		return SdlGpuState().PrepareOutputSurfaceFrame(frame.composition, outputSurface, stats);
	}

	void Present() override
	{
		SdlGpuState().Present();
	}
};

#endif

#if !DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
bool LoggedUnavailableBuild = false;
#endif

} // namespace

bool SdlGpuPaletteCompositorBuildAvailable()
{
	return DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE != 0;
}

bool SdlGpuPaletteCompositorRequested()
{
	return SdlGpuPaletteCompositorAllowedByRuntime();
}

bool SdlGpuPaletteCompositorWindowRequested()
{
	return SdlGpuPaletteCompositorRequested() && SdlGpuPaletteCompositorBuildAvailable();
}

AcceleratedCompositorWindowFlags ConfigureSdlGpuPaletteCompositorWindow()
{
	return 0;
}

bool ReinitializeSdlGpuPaletteCompositor(SDL_Window *window)
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	if (!SdlGpuPaletteCompositorRequested())
		return false;
	return SdlGpuState().Initialize(window);
#else
	(void)window;
	return false;
#endif
}

bool SdlGpuPaletteCompositorIsActive()
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	return SdlGpuState().IsAvailable();
#else
	return false;
#endif
}

bool SdlGpuPaletteCompositorGpuCursorOverlayAvailable()
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	return SdlGpuState().GpuCursorOverlayAvailable();
#else
	return false;
#endif
}

bool SdlGpuPaletteCompositorGpuAutomapOverlayAvailable()
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	return SdlGpuState().GpuAutomapOverlayAvailable();
#else
	return false;
#endif
}

void SetSdlGpuPaletteCompositorCursorOverlay(const SdlGpuPaletteCursorOverlay &overlay)
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	SdlGpuState().SetCursorOverlay(overlay);
#else
	(void)overlay;
#endif
}

void ClearSdlGpuPaletteCompositorCursorOverlay()
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	SdlGpuState().ClearCursorOverlay();
#endif
}

void SetSdlGpuPaletteCompositorAutomapOverlay(RenderAutomapOverlayView overlay)
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	SdlGpuState().SetAutomapOverlay(overlay);
#else
	(void)overlay;
#endif
}

void SetSdlGpuPaletteCompositorAutomapOverlayOffset(const Displacement offset)
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	SdlGpuState().SetAutomapOverlayOffset(offset);
#else
	(void)offset;
#endif
}

void SetSdlGpuPaletteCompositorAutomapOverlayRejectRects(const Rectangle *rects, const std::size_t count)
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	SdlGpuState().SetAutomapOverlayRejectRects(rects, count);
#else
	(void)rects;
	(void)count;
#endif
}

void SetSdlGpuPaletteCompositorAutomapPlayerOverlay(RenderAutomapOverlayView overlay)
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	SdlGpuState().SetAutomapPlayerOverlay(overlay);
#else
	(void)overlay;
#endif
}

void ClearSdlGpuPaletteCompositorAutomapOverlay()
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	SdlGpuState().ClearAutomapOverlay();
#endif
}

void ShutdownSdlGpuPaletteCompositor()
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	SdlGpuState().Destroy();
#endif
}

std::unique_ptr<IFrameCompositorBackend> CreateSdlGpuPaletteCompositorBackend()
{
#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
	if (!SdlGpuState().IsAvailable())
		return nullptr;
	return CreateAcceleratedPaletteCompositorBackend(std::make_unique<SdlGpuPalettePresenter>());
#else
	if (SdlGpuPaletteCompositorAllowedByRuntime() && !SdlGpuPaletteCompositorBuildAvailable() && !LoggedUnavailableBuild) {
		Log("SDL_GPU palette compositor was selected but is not built; falling back to CPU palette compositor");
		LoggedUnavailableBuild = true;
	}
	return nullptr;
#endif
}

} // namespace devilution
