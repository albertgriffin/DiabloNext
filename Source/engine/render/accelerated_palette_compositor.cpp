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

#include "utils/log.hpp"

namespace devilution {
namespace {

NeutralCompositionLightingInputs &GlobalNeutralLightingInputs()
{
	static NeutralCompositionLightingInputs inputs;
	return inputs;
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
		return PrepareNeutralCompositionLightingInputs(logicalSize);
	}

	std::unique_ptr<IAcceleratedPalettePresenter> presenter_;
	std::unique_ptr<IFrameCompositorBackend> cpuFallback_;
	const CompositionLightingInputs *lightingInputs_ = nullptr;
	bool loggedCpuFallback_ = false;
	bool hasDirectPresentationFrame_ = false;
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
	return &inputs_;
}

const CompositionLightingInputs *NeutralCompositionLightingInputs::Get() const
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

std::unique_ptr<IFrameCompositorBackend> CreateAcceleratedPaletteCompositorBackend(std::unique_ptr<IAcceleratedPalettePresenter> presenter, const CompositionLightingInputs *lightingInputs)
{
	if (presenter == nullptr || !presenter->IsAvailable())
		return nullptr;
	return std::make_unique<AcceleratedPaletteCompositorBackend>(std::move(presenter), lightingInputs);
}

} // namespace devilution
