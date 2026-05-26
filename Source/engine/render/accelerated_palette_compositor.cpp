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
#include <vector>

#include "engine/render/render_layer.hpp"
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

ClassicCompositionLightingInputs &GlobalClassicLightingInputs()
{
	static ClassicCompositionLightingInputs inputs;
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

[[nodiscard]] bool RenderLayerMapContains(const RenderLayerMapView &renderLayerMap, const Size size, const Point position)
{
	return renderLayerMap.pixels != nullptr
	    && renderLayerMap.width >= size.width
	    && renderLayerMap.height >= size.height
	    && renderLayerMap.pitch >= renderLayerMap.width
	    && position.x >= 0
	    && position.y >= 0
	    && position.x < renderLayerMap.width
	    && position.y < renderLayerMap.height;
}

[[nodiscard]] bool WorldEffectsAffectLayer(const uint8_t layerId)
{
	switch (static_cast<RenderLayer>(layerId)) {
	case RenderLayer::World:
	case RenderLayer::WorldOverlay:
		return true;
	case RenderLayer::Interface:
	case RenderLayer::Cursor:
	case RenderLayer::Debug:
	case RenderLayer::Count:
		return false;
	}
	return layerId == UnknownRenderLayerId;
}

[[nodiscard]] bool WorldEffectsAffectPixel(const RenderLayerMapView &renderLayerMap, const Size size, const Point position)
{
	if (!RenderLayerMapContains(renderLayerMap, size, position))
		return true;
	const auto *row = renderLayerMap.pixels + static_cast<size_t>(position.y) * renderLayerMap.pitch;
	return WorldEffectsAffectLayer(row[position.x]);
}

[[nodiscard]] bool RenderLayerMapCovers(const RenderLayerMapView &renderLayerMap, const Size size)
{
	return renderLayerMap.pixels != nullptr
	    && renderLayerMap.width >= size.width
	    && renderLayerMap.height >= size.height
	    && renderLayerMap.pitch >= renderLayerMap.width;
}

[[nodiscard]] bool ClassicLightingAffectsLayer(const uint8_t layerId)
{
	return static_cast<RenderLayer>(layerId) == RenderLayer::World;
}

[[nodiscard]] uint8_t ClassicNeutralShadowForPixel(const RenderLayerMapView &renderLayerMap, const int x, const int y)
{
	const auto *row = renderLayerMap.pixels + static_cast<size_t>(y) * renderLayerMap.pitch;
	return ClassicLightingAffectsLayer(row[x]) ? 0 : 255;
}

[[nodiscard]] Rectangle ClipRectToSize(Rectangle rect, const Size size)
{
	const int x0 = std::clamp(rect.position.x, 0, size.width);
	const int y0 = std::clamp(rect.position.y, 0, size.height);
	const int x1 = std::clamp(rect.position.x + rect.size.width, 0, size.width);
	const int y1 = std::clamp(rect.position.y + rect.size.height, 0, size.height);
	return { { x0, y0 }, { std::max(x1 - x0, 0), std::max(y1 - y0, 0) } };
}

[[nodiscard]] bool IsEmpty(const Rectangle &rect)
{
	return rect.size.width <= 0 || rect.size.height <= 0;
}

[[nodiscard]] bool ClassicLightMapContains(const RenderClassicLightMapView &classicLightMap, const Size size, const Point position)
{
	if (classicLightMap.storesDungeonGrid) {
		return classicLightMap.lightLevelPixels != nullptr
		    && classicLightMap.width > 0
		    && classicLightMap.height > 0
		    && classicLightMap.pitch >= classicLightMap.width
		    && classicLightMap.viewportHeight > 0;
	}
	return classicLightMap.lightLevelPixels != nullptr
	    && classicLightMap.width >= size.width
	    && classicLightMap.height >= size.height
	    && classicLightMap.pitch >= classicLightMap.width
	    && position.x >= 0
	    && position.y >= 0
	    && position.x < classicLightMap.width
	    && position.y < classicLightMap.height;
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

	bool CanConsumeClassicLightMapDirectly() const override
	{
		return PresenterAvailable()
		    && *GetOptions().Experimental.renderAcceleratedClassicLighting
		    && *GetOptions().Experimental.renderFrameCompositorBackend == RenderFrameCompositorBackend::SdlGpuPalette;
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
		const CompositionLightingInputs *lightingInputs = LightingInputsForIndexedFrame(frame);
		if (!presenter_->PrepareIndexedFrame({ frame, lightingInputs, rects }, stats)) {
			const std::vector<Rectangle> fallbackRects = CanConsumeClassicLightMapDirectly() && frame.classicLightMap.lightLevelPixels != nullptr
			    ? std::vector<Rectangle> { { { 0, 0 }, frame.logicalSize } }
			    : rects;
			const FrameCompositorBackendResult fallbackResult = cpuFallback_->Compose(frame, outputSurface, fallbackRects, stats);
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

	[[nodiscard]] const CompositionLightingInputs *LightingInputsForIndexedFrame(const CompositionFrame &frame)
	{
		if (lightingInputs_ != nullptr)
			return lightingInputs_;

		const RenderLightShadowDiagnosticMode diagnosticMode = *GetOptions().Experimental.renderLightShadowDiagnosticMode;
		if (*GetOptions().Experimental.renderAcceleratedClassicLighting && frame.classicLightMap.lightLevelPixels != nullptr) {
			if (!loggedClassicLighting_) {
				Log("{} using accelerated classic lighting", Name());
				loggedClassicLighting_ = true;
			}
			if (const CompositionLightingInputs *classicLighting = PrepareClassicCompositionLightingInputs(frame.logicalSize, frame.classicLightMap, frame.smoothLightSources, frame.renderLayerMap, frame.dirtyRects, diagnosticMode))
				return classicLighting;
		}
		loggedClassicLighting_ = false;
		if (diagnosticMode != RenderLightShadowDiagnosticMode::Off) {
			if (loggedDiagnosticMode_ != diagnosticMode) {
				Log("{} using development light/shadow diagnostic: {}", Name(), RenderLightShadowDiagnosticModeName(diagnosticMode));
				loggedDiagnosticMode_ = diagnosticMode;
			}
			return PrepareDevelopmentCompositionLightingInputs(frame.logicalSize, diagnosticMode, frame.renderLayerMap);
		}
		loggedDiagnosticMode_ = RenderLightShadowDiagnosticMode::Off;
		return PrepareNeutralCompositionLightingInputs(frame.logicalSize);
	}

	std::unique_ptr<IAcceleratedPalettePresenter> presenter_;
	std::unique_ptr<IFrameCompositorBackend> cpuFallback_;
	const CompositionLightingInputs *lightingInputs_ = nullptr;
	bool loggedCpuFallback_ = false;
	bool hasDirectPresentationFrame_ = false;
	bool loggedClassicLighting_ = false;
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
	inputs_.smoothLightSources = {};
	inputs_.classicLightFirstTile = {};
	inputs_.classicLightOffset = {};
	inputs_.classicLightViewportHeight = 0;
	inputs_.diagnosticMode = RenderLightShadowDiagnosticMode::Off;
	inputs_.lightStoresClassicLightLevels = false;
	inputs_.lightStoresDungeonGrid = false;
	inputs_.smoothPresentation = false;
	return &inputs_;
}

const CompositionLightingInputs *NeutralCompositionLightingInputs::Get() const
{
	return inputs_.light.IsValid() && inputs_.shadow.IsValid() ? &inputs_ : nullptr;
}

const CompositionLightingInputs *DevelopmentCompositionLightingInputs::Prepare(const Size size, const RenderLightShadowDiagnosticMode mode, const RenderLayerMapView renderLayerMap)
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
			const size_t lightOffset = static_cast<size_t>(x) * 4;
			if (!WorldEffectsAffectPixel(renderLayerMap, size, { x, y })) {
				lightRow[lightOffset + 0] = 255;
				lightRow[lightOffset + 1] = 255;
				lightRow[lightOffset + 2] = 255;
				lightRow[lightOffset + 3] = 255;
				shadowRow[x] = 0;
				continue;
			}

			const int dx = x - centerX;
			const int dy = y - centerY;
			const int distanceSquared = dx * dx + dy * dy;
			const int radial = distanceSquared >= radiusSquared ? 0 : 255 - distanceSquared * 255 / radiusSquared;
			const int stripePhase = static_cast<int>((static_cast<uint64_t>(x) + frame * 5) % 96);
			const int stripe = stripePhase < 24 ? 72 : 0;
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
	inputs_.smoothLightSources = {};
	inputs_.classicLightFirstTile = {};
	inputs_.classicLightOffset = {};
	inputs_.classicLightViewportHeight = 0;
	inputs_.diagnosticMode = mode;
	inputs_.lightStoresClassicLightLevels = false;
	inputs_.lightStoresDungeonGrid = false;
	inputs_.smoothPresentation = false;
	return &inputs_;
}

const CompositionLightingInputs *DevelopmentCompositionLightingInputs::Get() const
{
	return inputs_.light.IsValid() && inputs_.shadow.IsValid() ? &inputs_ : nullptr;
}

const CompositionLightingInputs *ClassicCompositionLightingInputs::Prepare(const Size size, const RenderClassicLightMapView classicLightMap, const RenderSmoothLightSourceView smoothLightSources, const RenderLayerMapView renderLayerMap, const DirtyRectList &dirtyRects, const RenderLightShadowDiagnosticMode diagnosticMode)
{
	if (size.width <= 0 || size.height <= 0 || !ClassicLightMapContains(classicLightMap, size, { 0, 0 })) {
		size_ = {};
		inputs_ = {};
		shadowPixels_.clear();
		shadowMaskInitialized_ = false;
		shadowMaskUsesLayerMap_ = false;
		shadowLayerMapVersion_ = 0;
		return nullptr;
	}

	size_ = size;
	const auto width = static_cast<size_t>(size.width);
	const auto height = static_cast<size_t>(size.height);
	const size_t shadowPitch = width;
	if (shadowPixels_.size() != shadowPitch * height) {
		shadowPixels_.assign(shadowPitch * height, 0);
		shadowMaskInitialized_ = false;
	}

	DirtyRectList shadowDirtyRects;
	if (classicLightMap.storesDungeonGrid) {
		const bool useExactLayerMask = RenderLayerMapCovers(renderLayerMap, size);
		const auto updateRect = [&](Rectangle rect) {
			rect = ClipRectToSize(rect, size);
			if (IsEmpty(rect))
				return;
			for (int y = rect.position.y; y < rect.position.y + rect.size.height; y++) {
				auto *shadowRow = shadowPixels_.data() + static_cast<size_t>(y) * shadowPitch;
				for (int x = rect.position.x; x < rect.position.x + rect.size.width; x++) {
					shadowRow[x] = useExactLayerMask ? ClassicNeutralShadowForPixel(renderLayerMap, x, y) : 0;
				}
			}
		};
		const auto updateFullFrame = [&]() {
			updateRect({ { 0, 0 }, size });
			shadowDirtyRects.fullFrame = true;
		};
		const bool needsInitialOrModeReset = !shadowMaskInitialized_ || shadowMaskUsesLayerMap_ != useExactLayerMask;
		if (needsInitialOrModeReset) {
			updateFullFrame();
		} else if (useExactLayerMask && renderLayerMap.version != shadowLayerMapVersion_) {
			if (renderLayerMap.dirtyFullFrame || renderLayerMap.dirtyRects == nullptr) {
				updateFullFrame();
			} else {
				for (size_t i = 0; i < renderLayerMap.dirtyRectCount; i++) {
					Rectangle rect = ClipRectToSize(renderLayerMap.dirtyRects[i], size);
					if (IsEmpty(rect))
						continue;
					updateRect(rect);
					shadowDirtyRects.rects.push_back(rect);
				}
				if (shadowDirtyRects.rects.empty())
					updateFullFrame();
			}
		}
		if (shadowDirtyRects.fullFrame || !shadowDirtyRects.rects.empty()) {
			if (shadowMaskInitialized_)
				shadowVersion_++;
			shadowLayerMapVersion_ = useExactLayerMask ? renderLayerMap.version : 0;
		}
		shadowMaskUsesLayerMap_ = useExactLayerMask;
		shadowMaskInitialized_ = true;
	} else {
		const bool hadNonNeutralShadow = std::any_of(shadowPixels_.begin(), shadowPixels_.end(), [](uint8_t value) { return value != 0; });
		if (hadNonNeutralShadow) {
			std::fill(shadowPixels_.begin(), shadowPixels_.end(), 0);
			shadowDirtyRects.fullFrame = true;
			shadowVersion_++;
		}
		shadowMaskInitialized_ = false;
		shadowMaskUsesLayerMap_ = false;
		shadowLayerMapVersion_ = 0;
	}

	const DirtyRectList lightDirtyRects = classicLightMap.storesDungeonGrid ? FullFrameDirtyRects() : dirtyRects;
	inputs_.light = {
		classicLightMap.lightLevelPixels,
		classicLightMap.storesDungeonGrid ? Size { classicLightMap.width, classicLightMap.height } : size,
		classicLightMap.pitch,
		CompositionLightingBufferFormat::Alpha8,
		classicLightMap.version,
		lightDirtyRects,
	};
	inputs_.shadow = {
		shadowPixels_.data(),
		size,
		static_cast<int>(shadowPitch),
		CompositionLightingBufferFormat::Alpha8,
		shadowVersion_,
		shadowDirtyRects,
	};
	inputs_.smoothLightSources = smoothLightSources;
	inputs_.classicLightFirstTile = classicLightMap.firstTile;
	inputs_.classicLightOffset = classicLightMap.offset;
	inputs_.classicLightViewportHeight = classicLightMap.viewportHeight;
	inputs_.diagnosticMode = diagnosticMode;
	inputs_.lightStoresClassicLightLevels = !classicLightMap.storesIntensity;
	inputs_.lightStoresDungeonGrid = classicLightMap.storesDungeonGrid;
	inputs_.smoothPresentation = smoothLightSources.sources != nullptr && smoothLightSources.count > 0;
	return &inputs_;
}

const CompositionLightingInputs *ClassicCompositionLightingInputs::Get() const
{
	return inputs_.light.IsValid() && inputs_.shadow.IsValid() ? &inputs_ : nullptr;
}

bool AcceleratedPaletteFrameRequiresCpuPixels(const CompositionFrame &frame)
{
	return frame.diagnosticTransform
	    || frame.renderLayerDiagnosticMode != RenderLayerDiagnosticMode::Off
	    || frame.renderWorldMaskDiagnosticMode != RenderWorldMaskDiagnosticMode::Off
	    || frame.renderWorldProxyDiagnosticMode != RenderWorldProxyDiagnosticMode::Off;
}

const CompositionLightingInputs *PrepareNeutralCompositionLightingInputs(const Size size)
{
	return GlobalNeutralLightingInputs().Prepare(size);
}

const CompositionLightingInputs *PrepareDevelopmentCompositionLightingInputs(const Size size, const RenderLightShadowDiagnosticMode mode, const RenderLayerMapView renderLayerMap)
{
	return GlobalDevelopmentLightingInputs().Prepare(size, mode, renderLayerMap);
}

const CompositionLightingInputs *PrepareClassicCompositionLightingInputs(const Size size, const RenderClassicLightMapView classicLightMap, const RenderSmoothLightSourceView smoothLightSources, const RenderLayerMapView renderLayerMap, const DirtyRectList &dirtyRects, const RenderLightShadowDiagnosticMode diagnosticMode)
{
	return GlobalClassicLightingInputs().Prepare(size, classicLightMap, smoothLightSources, renderLayerMap, dirtyRects, diagnosticMode);
}

std::unique_ptr<IFrameCompositorBackend> CreateAcceleratedPaletteCompositorBackend(std::unique_ptr<IAcceleratedPalettePresenter> presenter, const CompositionLightingInputs *lightingInputs)
{
	if (presenter == nullptr || !presenter->IsAvailable())
		return nullptr;
	return std::make_unique<AcceleratedPaletteCompositorBackend>(std::move(presenter), lightingInputs);
}

} // namespace devilution
