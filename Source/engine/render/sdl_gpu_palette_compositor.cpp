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

constexpr SDL_GPUShaderFormat SupportedShaderFormats = SDL_GPU_SHADERFORMAT_SPIRV
    | SDL_GPU_SHADERFORMAT_DXBC
    | SDL_GPU_SHADERFORMAT_DXIL
    | SDL_GPU_SHADERFORMAT_MSL
    | SDL_GPU_SHADERFORMAT_METALLIB;

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
		ReleaseTexture();
		ReleaseTransferBuffer();
		if (windowClaimed_ && device_ != nullptr && window_ != nullptr)
			SDL_ReleaseWindowFromGPUDevice(device_, window_);
		if (device_ != nullptr)
			SDL_DestroyGPUDevice(device_);
		device_ = nullptr;
		window_ = nullptr;
		windowClaimed_ = false;
		available_ = false;
		hasPendingOutput_ = false;
		outputTextureWidth_ = 0;
		outputTextureHeight_ = 0;
		transferBufferSize_ = 0;
		lastPresentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
		presentModeInitialized_ = false;
		loggedIndexedDeferral_ = false;
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
		(void)frame;
		if (!loggedIndexedDeferral_) {
			Log("SDL_GPU palette compositor indexed palette path is not implemented yet; using CPU pixel upload fallback");
			loggedIndexedDeferral_ = true;
		}
		return false;
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
		hasPendingOutput_ = true;
		return true;
	}

	void Present()
	{
		if (!available_ || !hasPendingOutput_ || pendingOutputSize_.width <= 0 || pendingOutputSize_.height <= 0)
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

private:
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
	SDL_GPUTransferBuffer *transferBuffer_ = nullptr;
	bool windowClaimed_ = false;
	bool available_ = false;
	bool hasPendingOutput_ = false;
	bool presentModeInitialized_ = false;
	bool loggedIndexedDeferral_ = false;
	int outputTextureWidth_ = 0;
	int outputTextureHeight_ = 0;
	uint32_t transferBufferSize_ = 0;
	SDL_GPUPresentMode lastPresentMode_ = SDL_GPU_PRESENTMODE_VSYNC;
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
