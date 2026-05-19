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

constexpr char PaletteVertexShaderMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut
{
	float4 position [[position]];
	float2 uv;
};

vertex VertexOut main0(uint vertexId [[vertex_id]])
{
	constexpr uint verts[6] = { 0, 1, 2, 0, 2, 3 };
	constexpr float2 uvs[4] = {
		float2(0.0, 0.0),
		float2(1.0, 0.0),
		float2(1.0, 1.0),
		float2(0.0, 1.0),
	};
	constexpr float2 positions[4] = {
		float2(-1.0, 1.0),
		float2(1.0, 1.0),
		float2(1.0, -1.0),
		float2(-1.0, -1.0),
	};

	const uint vert = verts[vertexId];
	VertexOut out;
	out.position = float4(positions[vert], 0.0, 1.0);
	out.uv = uvs[vert];
	return out;
}
)";

constexpr char PaletteFragmentShaderMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut
{
	float4 position [[position]];
	float2 uv;
};

fragment float4 main0(VertexOut in [[stage_in]],
	texture2d<float> indexTexture [[texture(0)]],
	texture2d<float> paletteTexture [[texture(1)]],
	sampler indexSampler [[sampler(0)]],
	sampler paletteSampler [[sampler(1)]])
{
	const float indexValue = indexTexture.sample(indexSampler, in.uv).r;
	const float paletteIndex = floor(indexValue * 255.0 + 0.5);
	const float paletteU = (paletteIndex + 0.5) / 256.0;
	return paletteTexture.sample(paletteSampler, float2(paletteU, 0.5));
}
)";

constexpr SDL_GPUShaderFormat SupportedShaderFormats = SDL_GPU_SHADERFORMAT_MSL;

[[nodiscard]] int SurfaceBytesPerPixel(const SDL_Surface &surface)
{
	return SDL_BYTESPERPIXEL(surface.format);
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
		ReleaseTransferBuffer();
		ReleaseIndexedTransferBuffers();
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
		palettePipelineFailed_ = false;
		paletteTextureUploaded_ = false;
		uploadedPaletteVersion_ = 0;
		pendingMode_ = PendingMode::None;
		palettePipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
		paletteExpandedTextureWidth_ = 0;
		paletteExpandedTextureHeight_ = 0;
		pendingLogicalSize_ = {};
		pendingOutputSize_ = {};
		outputRgba_.clear();
	}

	[[nodiscard]] bool IsAvailable() const
	{
		return available_;
	}

	[[nodiscard]] bool PrepareIndexedFrame(const CompositionFrame &frame)
	{
		if (!EnsurePaletteRenderResources()
		    || !EnsurePaletteExpandedTexture(frame.logicalSize.width, frame.logicalSize.height)
		    || !UploadIndexedInputs(frame)) {
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
		pendingLogicalSize_ = frame.logicalSize;
		pendingOutputSize_ = {};
		pendingMode_ = PendingMode::IndexedPalette;
		return true;
	}

	[[nodiscard]] bool PrepareOutputSurfaceFrame(const CompositionFrame &frame, SDL_Surface &outputSurface)
	{
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
		};
		SDL_SetGPUViewport(renderPass, &gpuViewport);
		SDL_BindGPUGraphicsPipeline(renderPass, palettePipeline_);
		SDL_BindGPUFragmentSamplers(renderPass, 0, samplers, 2);
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

	[[nodiscard]] SDL_GPUShader *CreateMslShader(const char *source, const size_t sourceSize, const SDL_GPUShaderStage stage, const uint32_t samplerCount)
	{
		const SDL_GPUShaderCreateInfo createInfo {
			sourceSize,
			reinterpret_cast<const uint8_t *>(source),
			"main0",
			SDL_GPU_SHADERFORMAT_MSL,
			stage,
			samplerCount,
			0,
			0,
			0,
			0,
		};
		SDL_GPUShader *shader = SDL_CreateGPUShader(device_, &createInfo);
		if (shader == nullptr)
			Log("SDL_GPU palette compositor could not create MSL shader: {}", SDL_GetError());
		return shader;
	}

	[[nodiscard]] bool EnsurePalettePipeline()
	{
		if (palettePipelineFailed_)
			return false;

		const SDL_GPUShaderFormat supportedFormats = SDL_GetGPUShaderFormats(device_);
		if ((supportedFormats & SDL_GPU_SHADERFORMAT_MSL) == 0) {
			Log("SDL_GPU palette compositor requires MSL shaders in this spike; falling back to CPU palette composition");
			palettePipelineFailed_ = true;
			return false;
		}

		if (palettePipeline_ != nullptr && palettePipelineFormat_ == PaletteExpandedTextureFormat)
			return true;

		if (palettePipeline_ != nullptr && device_ != nullptr)
			SDL_ReleaseGPUGraphicsPipeline(device_, palettePipeline_);
		palettePipeline_ = nullptr;
		palettePipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;

		SDL_GPUShader *vertexShader = CreateMslShader(PaletteVertexShaderMsl, sizeof(PaletteVertexShaderMsl) - 1, SDL_GPU_SHADERSTAGE_VERTEX, 0);
		if (vertexShader == nullptr) {
			palettePipelineFailed_ = true;
			return false;
		}
		SDL_GPUShader *fragmentShader = CreateMslShader(PaletteFragmentShaderMsl, sizeof(PaletteFragmentShaderMsl) - 1, SDL_GPU_SHADERSTAGE_FRAGMENT, 2);
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

	[[nodiscard]] bool CopyIndexBufferToTransferBuffer(const IndexBufferView &indexBuffer, const Size logicalSize, const uint32_t uploadSize)
	{
		void *mapped = SDL_MapGPUTransferBuffer(device_, indexTransferBuffer_, true);
		if (mapped == nullptr) {
			Log("SDL_GPU palette compositor could not map index transfer buffer: {}", SDL_GetError());
			return false;
		}

		auto *dst = static_cast<uint8_t *>(mapped);
		std::memset(dst, 0, uploadSize);
		for (int y = 0; y < logicalSize.height; y++) {
			const auto rowOffset = static_cast<ptrdiff_t>(y) * indexBuffer.pitch;
			std::memcpy(dst + rowOffset, indexBuffer.pixels + rowOffset, static_cast<size_t>(logicalSize.width));
		}
		SDL_UnmapGPUTransferBuffer(device_, indexTransferBuffer_);
		return true;
	}

	[[nodiscard]] bool CopyPaletteToTransferBuffer(const PaletteSnapshot &palette)
	{
		void *mapped = SDL_MapGPUTransferBuffer(device_, paletteTransferBuffer_, true);
		if (mapped == nullptr) {
			Log("SDL_GPU palette compositor could not map palette transfer buffer: {}", SDL_GetError());
			return false;
		}

		auto *dst = static_cast<uint8_t *>(mapped);
		for (size_t i = 0; i < palette.colors.size(); i++) {
			dst[i * 4 + 0] = palette.colors[i].r;
			dst[i * 4 + 1] = palette.colors[i].g;
			dst[i * 4 + 2] = palette.colors[i].b;
			dst[i * 4 + 3] = palette.colors[i].a;
		}
		SDL_UnmapGPUTransferBuffer(device_, paletteTransferBuffer_);
		return true;
	}

	[[nodiscard]] bool UploadIndexedInputs(const CompositionFrame &frame)
	{
		if (!available_ || frame.indexBuffer.pixels == nullptr || frame.logicalSize.width <= 0 || frame.logicalSize.height <= 0)
			return false;
		if (frame.indexBuffer.width < frame.logicalSize.width || frame.indexBuffer.height < frame.logicalSize.height || frame.indexBuffer.pitch < frame.logicalSize.width)
			return false;

		const uint64_t uploadSize64 = static_cast<uint64_t>(frame.indexBuffer.pitch) * static_cast<uint64_t>(frame.logicalSize.height);
		if (uploadSize64 > std::numeric_limits<uint32_t>::max())
			return false;
		const uint32_t indexUploadSize = static_cast<uint32_t>(uploadSize64);

		if (!EnsureIndexTexture(frame.logicalSize.width, frame.logicalSize.height))
			return false;
		if (!EnsurePaletteTexture())
			return false;
		if (!EnsureIndexedTransferBuffer(indexUploadSize))
			return false;
		if (!EnsurePaletteTransferBuffer())
			return false;
		if (!CopyIndexBufferToTransferBuffer(frame.indexBuffer, frame.logicalSize, indexUploadSize))
			return false;

		const bool uploadPalette = !paletteTextureUploaded_ || uploadedPaletteVersion_ != frame.palette.version;
		if (uploadPalette && !CopyPaletteToTransferBuffer(frame.palette))
			return false;

		SDL_GPUCommandBuffer *commandBuffer = SDL_AcquireGPUCommandBuffer(device_);
		if (commandBuffer == nullptr) {
			Log("SDL_GPU palette compositor could not acquire indexed upload command buffer: {}", SDL_GetError());
			return false;
		}

		SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(commandBuffer);
		if (copyPass == nullptr) {
			Log("SDL_GPU palette compositor could not begin indexed upload copy pass: {}", SDL_GetError());
			(void)SDL_CancelGPUCommandBuffer(commandBuffer);
			return false;
		}

		const SDL_GPUTextureTransferInfo indexSource {
			indexTransferBuffer_,
			0,
			static_cast<uint32_t>(frame.indexBuffer.pitch),
			static_cast<uint32_t>(frame.logicalSize.height),
		};
		const SDL_GPUTextureRegion indexDestination {
			indexTexture_,
			0,
			0,
			0,
			0,
			0,
			static_cast<uint32_t>(frame.logicalSize.width),
			static_cast<uint32_t>(frame.logicalSize.height),
			1,
		};
		SDL_UploadToGPUTexture(copyPass, &indexSource, &indexDestination, true);

		if (uploadPalette) {
			const SDL_GPUTextureTransferInfo paletteSource {
				paletteTransferBuffer_,
				0,
				static_cast<uint32_t>(PaletteTextureWidth),
				static_cast<uint32_t>(PaletteTextureHeight),
			};
			const SDL_GPUTextureRegion paletteDestination {
				paletteTexture_,
				0,
				0,
				0,
				0,
				0,
				static_cast<uint32_t>(PaletteTextureWidth),
				static_cast<uint32_t>(PaletteTextureHeight),
				1,
			};
			SDL_UploadToGPUTexture(copyPass, &paletteSource, &paletteDestination, true);
		}
		SDL_EndGPUCopyPass(copyPass);

		if (!SDL_SubmitGPUCommandBuffer(commandBuffer)) {
			Log("SDL_GPU palette compositor could not submit indexed upload command buffer: {}", SDL_GetError());
			return false;
		}

		if (uploadPalette) {
			paletteTextureUploaded_ = true;
			uploadedPaletteVersion_ = frame.palette.version;
		}
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
	SDL_GPUGraphicsPipeline *palettePipeline_ = nullptr;
	SDL_GPUSampler *indexSampler_ = nullptr;
	SDL_GPUSampler *paletteSampler_ = nullptr;
	SDL_GPUTransferBuffer *transferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *indexTransferBuffer_ = nullptr;
	SDL_GPUTransferBuffer *paletteTransferBuffer_ = nullptr;
	bool windowClaimed_ = false;
	bool available_ = false;
	bool presentModeInitialized_ = false;
	bool loggedIndexedDeferral_ = false;
	bool loggedIndexedPresentation_ = false;
	bool palettePipelineFailed_ = false;
	bool paletteTextureUploaded_ = false;
	int outputTextureWidth_ = 0;
	int outputTextureHeight_ = 0;
	int indexTextureWidth_ = 0;
	int indexTextureHeight_ = 0;
	int paletteExpandedTextureWidth_ = 0;
	int paletteExpandedTextureHeight_ = 0;
	uint32_t transferBufferSize_ = 0;
	uint32_t indexTransferBufferSize_ = 0;
	uint64_t uploadedPaletteVersion_ = 0;
	SDL_GPUPresentMode lastPresentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
	SDL_GPUTextureFormat palettePipelineFormat_ = SDL_GPU_TEXTUREFORMAT_INVALID;
	PendingMode pendingMode_ = PendingMode::None;
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

	bool PrepareIndexedFrame(const AcceleratedPaletteFrame &frame) override
	{
		return SdlGpuState().PrepareIndexedFrame(frame.composition);
	}

	bool PrepareOutputSurfaceFrame(const AcceleratedPaletteFrame &frame, SDL_Surface &outputSurface) override
	{
		return SdlGpuState().PrepareOutputSurfaceFrame(frame.composition, outputSurface);
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
