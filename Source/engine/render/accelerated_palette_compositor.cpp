/**
 * @file accelerated_palette_compositor.cpp
 *
 * Production-facing accelerated palette compositor seam.
 */
#include "engine/render/accelerated_palette_compositor.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "options.h"
#include "utils/log.hpp"

namespace devilution {
namespace {

NeutralCompositionLightingInputs &GlobalNeutralLightingInputs()
{
	static NeutralCompositionLightingInputs inputs;
	return inputs;
}

DevelopmentCompositionLightingInputs &GlobalDevelopmentLightingInputs()
{
	static DevelopmentCompositionLightingInputs inputs;
	return inputs;
}

[[nodiscard]] uint8_t ClampByte(const int value)
{
	return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

[[nodiscard]] DirtyRectList FullFrameDirtyRects()
{
	DirtyRectList dirtyRects;
	dirtyRects.fullFrame = true;
	return dirtyRects;
}

class AcceleratedPaletteCompositorBackend final : public IFrameCompositorBackend {
public:
	explicit AcceleratedPaletteCompositorBackend(std::unique_ptr<IAcceleratedPalettePresenter> presenter, const CompositionLightingInputs *lightingInputs)
	    : presenter_(std::move(presenter))
	    , cpuFallback_(CreateCpuFrameCompositorBackend())
	    , lightingInputs_(lightingInputs)
	{
	}

	std::string_view Name() const override
	{
		return presenter_ != nullptr ? presenter_->Name() : "accelerated-palette-unavailable";
	}

	bool IsAvailable() const override
	{
		return cpuFallback_ != nullptr && cpuFallback_->IsAvailable();
	}

	bool CanRetainDirectPresentation() const override
	{
		return hasDirectPresentationFrame_ && PresenterAvailable();
	}

	FrameCompositorBackendResult Compose(const CompositionFrame &frame, SDL_Surface &outputSurface, const std::vector<Rectangle> &rects, RenderPerfCompositionStats &stats) override
	{
		if (cpuFallback_ == nullptr || !cpuFallback_->IsAvailable()) {
			hasDirectPresentationFrame_ = false;
			return FrameCompositorBackendResult::NoFrameProduced;
		}
		if (!PresenterAvailable()) {
			hasDirectPresentationFrame_ = false;
			return cpuFallback_->Compose(frame, outputSurface, rects, stats);
		}

		if (AcceleratedPaletteFrameRequiresCpuPixels(frame)) {
			const FrameCompositorBackendResult fallbackResult = cpuFallback_->Compose(frame, outputSurface, rects, stats);
			if (fallbackResult == FrameCompositorBackendResult::NoFrameProduced) {
				hasDirectPresentationFrame_ = false;
				return FrameCompositorBackendResult::NoFrameProduced;
			}
			if (!loggedCpuFallback_) {
				Log("{} using CPU pixel fallback for diagnostics", Name());
				loggedCpuFallback_ = true;
			}
			if (!presenter_->PrepareOutputSurfaceFrame({ frame, lightingInputs_, rects }, outputSurface, stats)) {
				hasDirectPresentationFrame_ = false;
				return fallbackResult;
			}
			hasDirectPresentationFrame_ = true;
			return FrameCompositorBackendResult::PreparedDirectPresentation;
		}

		stats.selectedThreadCount = std::max(stats.selectedThreadCount, 1);
		const CompositionLightingInputs *lightingInputs = LightingInputsForIndexedFrame(frame.logicalSize);
		if (!presenter_->PrepareIndexedFrame({ frame, lightingInputs, rects }, stats)) {
			const FrameCompositorBackendResult fallbackResult = cpuFallback_->Compose(frame, outputSurface, rects, stats);
			if (fallbackResult == FrameCompositorBackendResult::NoFrameProduced) {
				hasDirectPresentationFrame_ = false;
				return FrameCompositorBackendResult::NoFrameProduced;
			}
			if (PresenterAvailable() && presenter_->PrepareOutputSurfaceFrame({ frame, lightingInputs_, rects }, outputSurface, stats)) {
				hasDirectPresentationFrame_ = true;
				return FrameCompositorBackendResult::PreparedDirectPresentation;
			}
			hasDirectPresentationFrame_ = false;
			return fallbackResult;
		}
		hasDirectPresentationFrame_ = true;
		return FrameCompositorBackendResult::PreparedDirectPresentation;
	}

	void Present() override
	{
		if (PresenterAvailable())
			presenter_->Present();
	}

private:
	[[nodiscard]] bool PresenterAvailable() const
	{
		return presenter_ != nullptr && presenter_->IsAvailable();
	}

	[[nodiscard]] const CompositionLightingInputs *LightingInputsForIndexedFrame(const Size logicalSize)
	{
		if (lightingInputs_ != nullptr)
			return lightingInputs_;

		const RenderLightShadowDiagnosticMode diagnosticMode = *GetOptions().Experimental.renderLightShadowDiagnosticMode;
		if (diagnosticMode != RenderLightShadowDiagnosticMode::Off) {
			if (loggedDiagnosticMode_ != diagnosticMode) {
				Log("{} using development light/shadow diagnostic: {}", Name(), RenderLightShadowDiagnosticModeName(diagnosticMode));
				loggedDiagnosticMode_ = diagnosticMode;
			}
			return PrepareDevelopmentCompositionLightingInputs(logicalSize, diagnosticMode);
		}
		loggedDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
		return PrepareNeutralCompositionLightingInputs(logicalSize);
	}

	std::unique_ptr<IAcceleratedPalettePresenter> presenter_;
	std::unique_ptr<IFrameCompositorBackend> cpuFallback_;
	const CompositionLightingInputs *lightingInputs_ = nullptr;
	bool loggedCpuFallback_ = false;
	bool hasDirectPresentationFrame_ = false;
	RenderLightShadowDiagnosticMode loggedDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
};

} // namespace

const CompositionLightingInputs *NeutralCompositionLightingInputs::Prepare(const Size size)
{
	if (size.width <= 0 || size.height <= 0) {
		size_ = {};
		inputs_ = {};
		lightPixels_.clear();
		shadowPixels_.clear();
		return nullptr;
	}

	if (size_ == size && inputs_.light.IsValid() && inputs_.shadow.IsValid())
		return &inputs_;

	size_ = size;
	const auto width = static_cast<size_t>(size.width);
	const auto height = static_cast<size_t>(size.height);
	const size_t lightPitch = width * 4;
	const size_t shadowPitch = width;
	lightPixels_.assign(lightPitch * height, 255);
	shadowPixels_.assign(shadowPitch * height, 0);
	inputs_.light = {
		lightPixels_.data(),
		size,
		static_cast<int>(lightPitch),
		CompositionLightingBufferFormat::Rgba8,
		1,
		{},
	};
	inputs_.shadow = {
		shadowPixels_.data(),
		size,
		static_cast<int>(shadowPitch),
		CompositionLightingBufferFormat::Alpha8,
		1,
		{},
	};
	inputs_.diagnosticMode = RenderLightShadowDiagnosticMode::Off;
	return &inputs_;
}

const CompositionLightingInputs *NeutralCompositionLightingInputs::Get() const
{
	return inputs_.light.IsValid() && inputs_.shadow.IsValid() ? &inputs_ : nullptr;
}

const CompositionLightingInputs *DevelopmentCompositionLightingInputs::Prepare(const Size size, const RenderLightShadowDiagnosticMode mode)
{
	if (mode == RenderLightShadowDiagnosticMode::Off || size.width <= 0 || size.height <= 0) {
		size_ = {};
		inputs_ = {};
		lightPixels_.clear();
		shadowPixels_.clear();
		return nullptr;
	}

	size_ = size;
	const auto width = static_cast<size_t>(size.width);
	const auto height = static_cast<size_t>(size.height);
	const size_t lightPitch = width * 4;
	const size_t shadowPitch = width;
	lightPixels_.resize(lightPitch * height);
	shadowPixels_.resize(shadowPitch * height);

	const uint64_t frame = version_++;
	const int radius = std::max(24, std::min(size.width, size.height) / 3);
	const int travelWidth = std::max(1, size.width + radius * 2);
	const int centerX = static_cast<int>((frame * 7) % static_cast<uint64_t>(travelWidth)) - radius;
	const int centerY = size.height / 2 + static_cast<int>((static_cast<int>(frame % 121) - 60) * static_cast<int64_t>(size.height) / 420);
	const int radiusSquared = std::max(1, radius * radius);
	const int wedgeStart = size.width / 4 + static_cast<int>((frame * 3) % static_cast<uint64_t>(std::max(1, size.width / 2)));

	for (int y = 0; y < size.height; y++) {
		auto *lightRow = lightPixels_.data() + static_cast<size_t>(y) * lightPitch;
		auto *shadowRow = shadowPixels_.data() + static_cast<size_t>(y) * shadowPitch;
		for (int x = 0; x < size.width; x++) {
			const int dx = x - centerX;
			const int dy = y - centerY;
			const int distanceSquared = dx * dx + dy * dy;
			const int radial = distanceSquared >= radiusSquared ? 0 : 255 - distanceSquared * 255 / radiusSquared;
			const int stripePhase = static_cast<int>((static_cast<uint64_t>(x) + frame * 5) % 96);
			const int stripe = stripePhase < 24 ? 72 : 0;
			const size_t lightOffset = static_cast<size_t>(x) * 4;
			lightRow[lightOffset + 0] = ClampByte(112 + radial / 2 + stripe);
			lightRow[lightOffset + 1] = ClampByte(104 + radial * 3 / 4);
			lightRow[lightOffset + 2] = ClampByte(120 + radial + stripe / 2);
			lightRow[lightOffset + 3] = 255;

			int shadow = 0;
			if (x > wedgeStart && y < size.height / 2 + (x - wedgeStart) / 2)
				shadow += 96;
			if ((((x / 32) + (y / 32) + static_cast<int>(frame / 8)) & 1) != 0)
				shadow += 36;
			shadowRow[x] = ClampByte(std::min(shadow, 180));
		}
	}

	const DirtyRectList dirtyRects = FullFrameDirtyRects();
	inputs_.light = {
		lightPixels_.data(),
		size,
		static_cast<int>(lightPitch),
		CompositionLightingBufferFormat::Rgba8,
		frame,
		dirtyRects,
	};
	inputs_.shadow = {
		shadowPixels_.data(),
		size,
		static_cast<int>(shadowPitch),
		CompositionLightingBufferFormat::Alpha8,
		frame,
		dirtyRects,
	};
	inputs_.diagnosticMode = mode;
	return &inputs_;
}

const CompositionLightingInputs *DevelopmentCompositionLightingInputs::Get() const
{
	return inputs_.light.IsValid() && inputs_.shadow.IsValid() ? &inputs_ : nullptr;
}

bool AcceleratedPaletteFrameRequiresCpuPixels(const CompositionFrame &frame)
{
	return frame.diagnosticTransform
	    || frame.renderLayerDiagnosticMode != RenderLayerDiagnosticMode::Off
	    || frame.renderLayerMap.pixels != nullptr;
}

const CompositionLightingInputs *PrepareNeutralCompositionLightingInputs(const Size size)
{
	return GlobalNeutralLightingInputs().Prepare(size);
}

const CompositionLightingInputs *PrepareDevelopmentCompositionLightingInputs(const Size size, const RenderLightShadowDiagnosticMode mode)
{
	return GlobalDevelopmentLightingInputs().Prepare(size, mode);
}

std::unique_ptr<IFrameCompositorBackend> CreateAcceleratedPaletteCompositorBackend(std::unique_ptr<IAcceleratedPalettePresenter> presenter, const CompositionLightingInputs *lightingInputs)
{
	if (presenter == nullptr || !presenter->IsAvailable())
		return nullptr;
	return std::make_unique<AcceleratedPaletteCompositorBackend>(std::move(presenter), lightingInputs);
}

} // namespace devilution
