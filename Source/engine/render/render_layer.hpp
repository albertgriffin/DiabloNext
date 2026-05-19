/**
 * @file render_layer.hpp
 *
 * Conceptual render layer tracking for indexed renderer diagnostics.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

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
};

[[nodiscard]] RenderLayer CurrentRenderLayer();
[[nodiscard]] const RenderLayerFrameStats &GetRenderLayerFrameStats();
[[nodiscard]] RenderLayerMapView CurrentRenderLayerMapView();
void ResetRenderLayerFrameStats();
void BeginRenderLayerFrame(const Surface &surface, bool captureEnabled);
void BeginRenderLayer(RenderLayer layer);
void BeginRenderLayer(RenderLayer layer, Rectangle captureBounds);
void EndRenderLayer(RenderLayer layer);
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

class RenderLayerCaptureSuspension {
public:
	RenderLayerCaptureSuspension();
	~RenderLayerCaptureSuspension();

	RenderLayerCaptureSuspension(const RenderLayerCaptureSuspension &) = delete;
	RenderLayerCaptureSuspension &operator=(const RenderLayerCaptureSuspension &) = delete;
};

} // namespace devilution
