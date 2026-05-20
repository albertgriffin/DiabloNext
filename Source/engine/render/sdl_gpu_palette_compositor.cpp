/**
 * @file sdl_gpu_palette_compositor.cpp
 *
 * Optional SDL_GPU palette compositor target.
 */
#include "engine/render/sdl_gpu_palette_compositor.hpp"

#include <algorithm>
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
#include "headless_mode.hpp"
#include "options.h"
#include "utils/log.hpp"

#if defined(DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR) && defined(USE_SDL3) && !defined(USE_SDL1)
#include <SDL3/SDL_gpu.h>
#define DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE 1
#else
#define DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE 0
#endif

#if DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE
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

[[nodiscard]] bool SdlGpuPaletteCompositorBuildAvailable()
{
	return DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE != 0;
}

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
constexpr int PaletteTextureHeight = 1;
constexpr uint32_t PaletteUploadBytes = PaletteTextureWidth * PaletteTextureHeight * 4;
constexpr SDL_GPUTextureFormat PaletteExpandedTextureFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat LightTextureFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat ShadowTextureFormat = SDL_GPU_TEXTUREFORMAT_R8_UNORM;

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
	uint32_t padding[3] {};
};

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
		loggedDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
		palettePipelineFailed_ = false;
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
		palettePipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
		paletteExpandedTextureWidth_ = 0;
		paletteExpandedTextureHeight_ = 0;
		lightTextureWidth_ = 0;
		lightTextureHeight_ = 0;
		shadowTextureWidth_ = 0;
		shadowTextureHeight_ = 0;
		pendingLogicalSize_ = {};
		pendingOutputSize_ = {};
		outputRgba_.clear();
	}

	[[nodiscard]] bool IsAvailable() const
	{
		return available_;
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
		if (!EnsurePaletteExpandedTexture(pendingLogicalSize_.width, pendingLogicalSize_.height))
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
			static_cast<float>(pendingLogicalSize_.width),
			static_cast<float>(pendingLogicalSize_.height),
			0.F,
			1.F,
		};
		const SDL_GPUTextureSamplerBinding samplers[] {
			{ indexTexture_, indexSampler_ },
			{ paletteTexture_, paletteSampler_ },
			{ lightTexture_, indexSampler_ },
			{ shadowTexture_, indexSampler_ },
		};
		SDL_SetGPUViewport(renderPass, &gpuViewport);
		SDL_BindGPUGraphicsPipeline(renderPass, palettePipeline_);
		SDL_BindGPUFragmentSamplers(renderPass, 0, samplers, 4);
		const PaletteShaderUniforms uniforms {
			PaletteShaderDiagnosticView(pendingLightShadowDiagnosticMode_),
			{},
		};
		SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
		SDL_DrawGPUPrimitives(renderPass, 6, 1, 0, 0);
		SDL_EndGPURenderPass(renderPass);

		const Rectangle viewport = CalculateViewport({ static_cast<int>(swapchainWidth), static_cast<int>(swapchainHeight) }, pendingLogicalSize_);
		const SDL_GPUBlitInfo blit {
			{
			    paletteExpandedTexture_,
			    0,
			    0,
			    0,
			    0,
			    static_cast<uint32_t>(pendingLogicalSize_.width),
			    static_cast<uint32_t>(pendingLogicalSize_.height),
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
	}

	void ReleasePaletteExpandedTexture()
	{
		if (paletteExpandedTexture_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUTexture(device_, paletteExpandedTexture_);
		paletteExpandedTexture_ = nullptr;
		paletteExpandedTextureWidth_ = 0;
		paletteExpandedTextureHeight_ = 0;
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

	void ReleasePalettePipelineResources()
	{
		if (palettePipeline_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUGraphicsPipeline(device_, palettePipeline_);
		if (indexSampler_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUSampler(device_, indexSampler_);
		if (paletteSampler_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUSampler(device_, paletteSampler_);
		palettePipeline_ = nullptr;
		indexSampler_ = nullptr;
		paletteSampler_ = nullptr;
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

	[[nodiscard]] bool EnsureLightingTexture(SDL_GPUTexture *&texture, int &currentWidth, int &currentHeight, bool &textureUploaded, uint64_t &uploadedVersion, const Size size, const SDL_GPUTextureFormat format, const char *name)
	{
		if (texture != nullptr && currentWidth == size.width && currentHeight == size.height)
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
		return true;
	}

	[[nodiscard]] bool EnsureLightingTextures(const Size size)
	{
		return EnsureLightingTexture(lightTexture_, lightTextureWidth_, lightTextureHeight_, lightTextureUploaded_, uploadedLightVersion_, size, LightTextureFormat, "light")
		    && EnsureLightingTexture(shadowTexture_, shadowTextureWidth_, shadowTextureHeight_, shadowTextureUploaded_, uploadedShadowVersion_, size, ShadowTextureFormat, "shadow");
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
			SDL_UploadToGPUTexture(copyPass, &source, &destination, true);
		}
	}

	[[nodiscard]] bool LightingInputsAreUsable(const CompositionLightingInputs *lightingInputs, const Size logicalSize) const
	{
		if (lightingInputs == nullptr || !lightingInputs->light.IsValid() || !lightingInputs->shadow.IsValid())
			return false;
		if (lightingInputs->light.size != logicalSize || lightingInputs->shadow.size != logicalSize)
			return false;
		if (lightingInputs->light.format != CompositionLightingBufferFormat::Rgba8 || lightingInputs->shadow.format != CompositionLightingBufferFormat::Alpha8)
			return false;
		if (lightingInputs->light.pitch < logicalSize.width * 4 || lightingInputs->light.pitch % 4 != 0)
			return false;
		if (lightingInputs->shadow.pitch < logicalSize.width)
			return false;
		return true;
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
		CompositionAttachment paletteAttachment = paletteAttachmentPtr != nullptr
		    ? *paletteAttachmentPtr
		    : MakePaletteAttachment(composition.palette);
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
		if (!EnsureLightingTextures(composition.logicalSize))
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
	SDL_GPUGraphicsPipeline *palettePipeline_ = nullptr;
	SDL_GPUSampler *indexSampler_ = nullptr;
	SDL_GPUSampler *paletteSampler_ = nullptr;
	SDL_GPUTransferBuffer *transferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *indexTransferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *paletteTransferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *lightTransferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *shadowTransferBuffer_ = nullptr;
	bool windowClaimed_ = false;
	bool available_ = false;
	bool presentModeInitialized_ = false;
	bool loggedIndexedDeferral_ = false;
	bool loggedIndexedPresentation_ = false;
	bool loggedLightingInputs_ = false;
	RenderLightShadowDiagnosticMode loggedDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
	bool palettePipelineFailed_ = false;
	bool indexTextureUploaded_ = false;
	bool paletteTextureUploaded_ = false;
	bool lightTextureUploaded_ = false;
	bool shadowTextureUploaded_ = false;
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
	uint32_t transferBufferSize_ = 0;
	uint32_t indexTransferBufferSize_ = 0;
	uint32_t lightTransferBufferSize_ = 0;
	uint32_t shadowTransferBufferSize_ = 0;
	uint64_t uploadedIndexVersion_ = 0;
	uint64_t uploadedPaletteVersion_ = 0;
	uint64_t uploadedLightVersion_ = 0;
	uint64_t uploadedShadowVersion_ = 0;
	SDL_GPUPresentMode lastPresentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
	SDL_GPUTextureFormat palettePipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
	PendingMode pendingMode_ = PendingMode::None;
	RenderLightShadowDiagnosticMode pendingLightShadowDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
	Size pendingLogicalSize_ {};
	Size pendingOutputSize_ {};
	std::vector<uint8_t> outputRgba_;
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
