/**
 * @file render_layer.hpp
 *
 * Conceptual render layer tracking for indexed renderer diagnostics.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/rectangle.hpp"
#include "engine/render/render_layer_diagnostics.hpp"
#include "utils/attributes.h"

namespace devilution {

struct Surface;

enum class RenderLayer : uint8_t {
	World,
	WorldOverlay,
	Interface,
	Cursor,
	Debug,
	Count,
};

inline constexpr size_t RenderLayerCount = static_cast<size_t>(RenderLayer::Count);

struct RenderLayerStats {
	uint32_t enterCount = 0;
	uint32_t dirtyRectCount = 0;
};

struct RenderLayerFrameStats {
	std::array<RenderLayerStats, RenderLayerCount> layers {};
	RenderLayer currentLayer = RenderLayer::World;
	uint64_t stampedSpanCount = 0;
	uint64_t stampedPixelCount = 0;
	uint64_t worldMaskStampedSpanCount = 0;
	uint64_t worldMaskStampedPixelCount = 0;
	uint64_t worldProxyPrimitiveCount = 0;
	uint64_t worldProxyActorPrimitiveCount = 0;
	uint64_t worldProxyPixelCount = 0;
	uint64_t classicLightStampedSpanCount = 0;
	uint64_t classicLightStampedPixelCount = 0;
	uint64_t smoothLightSourceCount = 0;
};

[[nodiscard]] RenderLayer CurrentRenderLayer();
[[nodiscard]] RenderWorldMaterial CurrentRenderWorldMaterial();
[[nodiscard]] uint8_t CurrentRenderWorldMaskFlags();
[[nodiscard]] const RenderLayerFrameStats &GetRenderLayerFrameStats();
[[nodiscard]] RenderLayerMapView CurrentRenderLayerMapView();
[[nodiscard]] RenderWorldMaskMapView CurrentRenderWorldMaskMapView();
[[nodiscard]] RenderWorldProxyMapView CurrentRenderWorldProxyMapView();
[[nodiscard]] RenderClassicLightMapView CurrentRenderClassicLightMapView();
[[nodiscard]] MutableRenderClassicLightMapView CurrentMutableRenderClassicLightMapView();
[[nodiscard]] RenderSmoothLightSourceView CurrentRenderSmoothLightSourceView();
void SetRenderClassicLightGrid(const uint8_t *pixels, Size size, int pitch, Point firstTile, Displacement offset, int viewportHeight);
void SetRenderSmoothLightSources(std::vector<RenderSmoothLightSource> sources);
void MarkRenderClassicLightMapChanged(bool storesIntensity);
void ResetRenderLayerFrameStats();
void BeginRenderLayerFrame(const Surface &surface, bool captureEnabled, bool worldMaskCaptureEnabled = false, bool worldProxyCaptureEnabled = false, bool worldProxyActorOccludersEnabled = false, bool classicLightCaptureEnabled = false, bool classicLightGeneratedIntensityMap = false, bool classicLightGridEnabled = false, bool layerMapDefaultsToWorld = false);
void BeginRenderLayer(RenderLayer layer);
void BeginRenderLayer(RenderLayer layer, Rectangle captureBounds);
void EndRenderLayer(RenderLayer layer);
void MarkRenderLayerRect(RenderLayer layer, Rectangle rect);
void BeginRenderWorldMask(RenderWorldMaterial material, uint8_t flags);
void EndRenderWorldMask(RenderWorldMaterial material);
void BeginRenderClassicLight(uint8_t lightLevel);
void EndRenderClassicLight(uint8_t lightLevel);
void MarkRenderWorldProxyPrimitive(RenderWorldProxyPrimitive primitive, Rectangle bounds);
void MarkRenderWorldProxyFloorDiamond(Point position);
void MarkRenderWorldProxyTilePrimitive(RenderWorldProxyPrimitive primitive, Point position);
void MarkRenderWorldProxyActorBillboard(Rectangle bounds);
void RecordRenderLayerDirtyRect(RenderLayer layer);
[[nodiscard]] bool SaveRenderLayerMapRegion(Rectangle rect, uint8_t *destination, int destinationPitch);
bool RestoreRenderLayerMapRegion(Rectangle rect, const uint8_t *source, int sourcePitch);

extern DVL_API_FOR_TEST bool RenderLayerCaptureActive;

void MarkRenderLayerPixelSlow(const Surface &surface, Point position);
void MarkRenderLayerSpanSlow(const Surface &surface, Point position, int width);
void MarkRenderLayerRectSlow(const Surface &surface, Rectangle rect);
void MarkRenderLayerSpanSlow(const uint8_t *dst, int width);

inline void MarkRenderLayerPixel(const Surface &surface, const Point position)
{
	if (RenderLayerCaptureActive)
		MarkRenderLayerPixelSlow(surface, position);
}

inline void MarkRenderLayerSpan(const Surface &surface, const Point position, const int width)
{
	if (RenderLayerCaptureActive)
		MarkRenderLayerSpanSlow(surface, position, width);
}

inline void MarkRenderLayerRect(const Surface &surface, const Rectangle rect)
{
	if (RenderLayerCaptureActive)
		MarkRenderLayerRectSlow(surface, rect);
}

inline void MarkRenderLayerSpan(const uint8_t *dst, const int width)
{
	if (RenderLayerCaptureActive)
		MarkRenderLayerSpanSlow(dst, width);
}

class RenderLayerScope {
public:
	explicit RenderLayerScope(RenderLayer layer);
	RenderLayerScope(RenderLayer layer, Rectangle captureBounds);
	~RenderLayerScope();

	RenderLayerScope(const RenderLayerScope &) = delete;
	RenderLayerScope &operator=(const RenderLayerScope &) = delete;

private:
	RenderLayer layer_;
};

class RenderWorldMaskScope {
public:
	explicit RenderWorldMaskScope(RenderWorldMaterial material, uint8_t flags = 0);
	~RenderWorldMaskScope();

	RenderWorldMaskScope(const RenderWorldMaskScope &) = delete;
	RenderWorldMaskScope &operator=(const RenderWorldMaskScope &) = delete;

private:
	RenderWorldMaterial material_;
};

class RenderClassicLightScope {
public:
	explicit RenderClassicLightScope(uint8_t lightLevel);
	~RenderClassicLightScope();

	RenderClassicLightScope(const RenderClassicLightScope &) = delete;
	RenderClassicLightScope &operator=(const RenderClassicLightScope &) = delete;

private:
	uint8_t lightLevel_;
};

class RenderLayerCaptureSuspension {
public:
	RenderLayerCaptureSuspension();
	~RenderLayerCaptureSuspension();

	RenderLayerCaptureSuspension(const RenderLayerCaptureSuspension &) = delete;
	RenderLayerCaptureSuspension &operator=(const RenderLayerCaptureSuspension &) = delete;
};

} // namespace devilution
