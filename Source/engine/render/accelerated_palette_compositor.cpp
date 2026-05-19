/**
 * @file accelerated_palette_compositor.cpp
 *
 * Production-facing accelerated palette compositor seam.
 */
#include "engine/render/accelerated_palette_compositor.hpp"

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>

#include "utils/log.hpp"

namespace devilution {
namespace {

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

	FrameCompositorBackendResult Compose(const CompositionFrame &frame, SDL_Surface &outputSurface, const std::vector<Rectangle> &rects, RenderPerfCompositionStats &stats) override
	{
		if (cpuFallback_ == nullptr || !cpuFallback_->IsAvailable())
			return FrameCompositorBackendResult::None;
		if (!PresenterAvailable())
			return cpuFallback_->Compose(frame, outputSurface, rects, stats);

		if (AcceleratedPaletteFrameRequiresCpuPixels(frame)) {
			const FrameCompositorBackendResult fallbackResult = cpuFallback_->Compose(frame, outputSurface, rects, stats);
			if (fallbackResult == FrameCompositorBackendResult::None)
				return FrameCompositorBackendResult::None;
			if (!loggedCpuFallback_) {
				Log("{} using CPU pixel fallback for diagnostics", Name());
				loggedCpuFallback_ = true;
			}
			if (!presenter_->PrepareOutputSurfaceFrame({ frame, lightingInputs_ }, outputSurface))
				return fallbackResult;
			return FrameCompositorBackendResult::Presented;
		}

		stats.selectedThreadCount = std::max(stats.selectedThreadCount, 1);
		if (!presenter_->PrepareIndexedFrame({ frame, lightingInputs_ })) {
			const FrameCompositorBackendResult fallbackResult = cpuFallback_->Compose(frame, outputSurface, rects, stats);
			if (fallbackResult == FrameCompositorBackendResult::None)
				return FrameCompositorBackendResult::None;
			if (PresenterAvailable() && presenter_->PrepareOutputSurfaceFrame({ frame, lightingInputs_ }, outputSurface))
				return FrameCompositorBackendResult::Presented;
			return fallbackResult;
		}
		return FrameCompositorBackendResult::Presented;
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

	std::unique_ptr<IAcceleratedPalettePresenter> presenter_;
	std::unique_ptr<IFrameCompositorBackend> cpuFallback_;
	const CompositionLightingInputs *lightingInputs_ = nullptr;
	bool loggedCpuFallback_ = false;
};

} // namespace

bool AcceleratedPaletteFrameRequiresCpuPixels(const CompositionFrame &frame)
{
	return frame.diagnosticTransform
	    || frame.renderLayerDiagnosticMode != RenderLayerDiagnosticMode::Off
	    || frame.renderLayerMap.pixels != nullptr;
}

std::unique_ptr<IFrameCompositorBackend> CreateAcceleratedPaletteCompositorBackend(std::unique_ptr<IAcceleratedPalettePresenter> presenter, const CompositionLightingInputs *lightingInputs)
{
	if (presenter == nullptr || !presenter->IsAvailable())
		return nullptr;
	return std::make_unique<AcceleratedPaletteCompositorBackend>(std::move(presenter), lightingInputs);
}

} // namespace devilution
