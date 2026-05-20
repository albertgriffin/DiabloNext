/**
 * @file render_layer.cpp
 *
 * Conceptual render layer tracking for indexed renderer diagnostics.
 */
#include "engine/render/render_layer.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "engine/surface.hpp"

namespace devilution {
namespace {

struct CaptureBuffer {
	const void *surface = nullptr;
	const uint8_t *pixels = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0;
};

RenderLayerFrameStats FrameStats;
std::vector<RenderLayer> LayerStack;
bool CaptureEnabled = false;
int CaptureSuspensionDepth = 0;
CaptureBuffer ActiveBuffer;
CaptureBuffer LayerMapBuffer;
std::vector<uint8_t> LayerMap;

[[nodiscard]] size_t LayerIndex(const RenderLayer layer)
{
	const size_t index = static_cast<size_t>(layer);
	assert(index < RenderLayerCount);
	return index;
}

[[nodiscard]] bool SameCaptureBuffer(const CaptureBuffer &a, const CaptureBuffer &b)
{
	return a.surface == b.surface
	    && a.pixels == b.pixels
	    && a.width == b.width
	    && a.height == b.height
	    && a.pitch == b.pitch;
}

[[nodiscard]] size_t LayerMapSize()
{
	return static_cast<size_t>(ActiveBuffer.width) * static_cast<size_t>(ActiveBuffer.height);
}

[[nodiscard]] bool CanStamp()
{
	return RenderLayerCaptureActive
	    && ActiveBuffer.pixels != nullptr
	    && ActiveBuffer.width > 0
	    && ActiveBuffer.height > 0
	    && LayerMap.size() == LayerMapSize();
}

[[nodiscard]] bool CanAccessLayerMapRegion(const Rectangle &rect, const int pitch)
{
	return CaptureEnabled
	    && ActiveBuffer.pixels != nullptr
	    && rect.size.width > 0
	    && rect.size.height > 0
	    && pitch >= rect.size.width
	    && rect.position.x >= 0
	    && rect.position.y >= 0
	    && rect.position.x + rect.size.width <= ActiveBuffer.width
	    && rect.position.y + rect.size.height <= ActiveBuffer.height
	    && LayerMap.size() == LayerMapSize();
}

void RefreshCaptureActive()
{
	RenderLayerCaptureActive = CaptureEnabled && CaptureSuspensionDepth == 0;
}

void MarkLayerMapSpan(const size_t index, const int width)
{
	std::memset(LayerMap.data() + index, static_cast<uint8_t>(FrameStats.currentLayer), static_cast<size_t>(width));
}

[[nodiscard]] bool TryGetLayerMapSpan(const uint8_t *dst, int width, size_t &index, int &clippedWidth)
{
	if (!CanStamp() || dst == nullptr || width <= 0)
		return false;

	const uintptr_t address = reinterpret_cast<uintptr_t>(dst);
	const uintptr_t bufferBegin = reinterpret_cast<uintptr_t>(ActiveBuffer.pixels);
	const uintptr_t bufferEnd = bufferBegin + static_cast<uintptr_t>(ActiveBuffer.pitch) * static_cast<uintptr_t>(ActiveBuffer.height);
	if (address < bufferBegin || address >= bufferEnd)
		return false;

	const uintptr_t offset = address - bufferBegin;
	const int y = static_cast<int>(offset / static_cast<uintptr_t>(ActiveBuffer.pitch));
	const int x = static_cast<int>(offset % static_cast<uintptr_t>(ActiveBuffer.pitch));
	if (x < 0 || x >= ActiveBuffer.width || y < 0 || y >= ActiveBuffer.height)
		return false;

	clippedWidth = std::min(width, ActiveBuffer.width - x);
	if (clippedWidth <= 0)
		return false;

	index = static_cast<size_t>(y) * ActiveBuffer.width + x;
	return true;
}

[[nodiscard]] bool ClipSurfaceSpan(const Surface &surface, Point &position, int &width)
{
	if (surface.surface == nullptr || position.y < 0 || position.y >= surface.h() || width <= 0 || position.x >= surface.w() || position.x + width <= 0)
		return false;

	if (position.x < 0) {
		width += position.x;
		position.x = 0;
	}
	if (position.x + width > surface.w())
		width = surface.w() - position.x;
	return width > 0;
}

[[nodiscard]] bool ClipSurfaceRect(const Surface &surface, Rectangle &rect)
{
	if (surface.surface == nullptr || rect.size.width <= 0 || rect.size.height <= 0)
		return false;

	const int x0 = std::clamp(rect.position.x, 0, surface.w());
	const int y0 = std::clamp(rect.position.y, 0, surface.h());
	const int x1 = std::clamp(rect.position.x + rect.size.width, 0, surface.w());
	const int y1 = std::clamp(rect.position.y + rect.size.height, 0, surface.h());
	rect = { { x0, y0 }, { std::max(x1 - x0, 0), std::max(y1 - y0, 0) } };
	return rect.size.width > 0 && rect.size.height > 0;
}

} // namespace

bool RenderLayerCaptureActive = false;

void MarkRenderLayerSpanSlow(const uint8_t *dst, int width)
{
	size_t index = 0;
	int clippedWidth = 0;
	if (!TryGetLayerMapSpan(dst, width, index, clippedWidth))
		return;

	FrameStats.stampedSpanCount++;
	FrameStats.stampedPixelCount += static_cast<uint64_t>(clippedWidth);
	MarkLayerMapSpan(index, clippedWidth);
}

void MarkRenderLayerPixelSlow(const Surface &surface, const Point position)
{
	if (surface.surface == nullptr || !surface.InBounds(position))
		return;

	MarkRenderLayerSpanSlow(surface.at(position.x, position.y), 1);
}

void MarkRenderLayerSpanSlow(const Surface &surface, Point position, int width)
{
	if (!ClipSurfaceSpan(surface, position, width))
		return;

	MarkRenderLayerSpanSlow(surface.at(position.x, position.y), width);
}

void MarkRenderLayerRectSlow(const Surface &surface, Rectangle rect)
{
	if (!ClipSurfaceRect(surface, rect))
		return;

	for (int y = 0; y < rect.size.height; y++) {
		MarkRenderLayerSpanSlow(surface.at(rect.position.x, rect.position.y + y), rect.size.width);
	}
}

RenderLayer CurrentRenderLayer()
{
	return FrameStats.currentLayer;
}

const RenderLayerFrameStats &GetRenderLayerFrameStats()
{
	return FrameStats;
}

RenderLayerMapView CurrentRenderLayerMapView()
{
	if (!CaptureEnabled || LayerMap.empty())
		return {};

	return {
		LayerMap.data(),
		ActiveBuffer.width,
		ActiveBuffer.height,
		ActiveBuffer.width,
	};
}

void ResetRenderLayerFrameStats()
{
	FrameStats = {};
	LayerStack.clear();
	CaptureEnabled = false;
	CaptureSuspensionDepth = 0;
	RefreshCaptureActive();
	ActiveBuffer = {};
	LayerMapBuffer = {};
	LayerMap.clear();
}

void BeginRenderLayerFrame(const Surface &surface, const bool captureEnabled)
{
	FrameStats = {};
	LayerStack.clear();
	CaptureSuspensionDepth = 0;
	CaptureEnabled = captureEnabled && surface.surface != nullptr && surface.w() > 0 && surface.h() > 0;
	RefreshCaptureActive();
	if (!CaptureEnabled) {
		ActiveBuffer = {};
		LayerMapBuffer = {};
		LayerMap.clear();
		return;
	}

	ActiveBuffer = {
		surface.surface,
		surface.begin(),
		surface.w(),
		surface.h(),
		surface.pitch(),
	};

	const size_t layerMapSize = LayerMapSize();
	if (!SameCaptureBuffer(ActiveBuffer, LayerMapBuffer) || LayerMap.size() != layerMapSize) {
		LayerMap.assign(layerMapSize, UnknownRenderLayerId);
		LayerMapBuffer = ActiveBuffer;
	}
}

void BeginRenderLayer(const RenderLayer layer)
{
	BeginRenderLayer(layer, {});
}

void BeginRenderLayer(const RenderLayer layer, const Rectangle captureBounds)
{
	(void)captureBounds;
	LayerStack.push_back(FrameStats.currentLayer);
	FrameStats.currentLayer = layer;
	FrameStats.layers[LayerIndex(layer)].enterCount++;
}

void EndRenderLayer(const RenderLayer layer)
{
	assert(FrameStats.currentLayer == layer);
	if (LayerStack.empty()) {
		FrameStats.currentLayer = RenderLayer::World;
		return;
	}

	FrameStats.currentLayer = LayerStack.back();
	LayerStack.pop_back();
}

void RecordRenderLayerDirtyRect(const RenderLayer layer)
{
	FrameStats.layers[LayerIndex(layer)].dirtyRectCount++;
}

bool SaveRenderLayerMapRegion(const Rectangle rect, uint8_t *destination, const int destinationPitch)
{
	if (destination == nullptr || !CanAccessLayerMapRegion(rect, destinationPitch))
		return false;

	for (int y = 0; y < rect.size.height; y++) {
		const size_t layerOffset = static_cast<size_t>(rect.position.y + y) * ActiveBuffer.width + rect.position.x;
		std::memcpy(destination + static_cast<size_t>(y) * destinationPitch, LayerMap.data() + layerOffset, rect.size.width);
	}
	return true;
}

bool RestoreRenderLayerMapRegion(const Rectangle rect, const uint8_t *source, const int sourcePitch)
{
	if (source == nullptr || !CanAccessLayerMapRegion(rect, sourcePitch))
		return false;

	for (int y = 0; y < rect.size.height; y++) {
		const size_t layerOffset = static_cast<size_t>(rect.position.y + y) * ActiveBuffer.width + rect.position.x;
		std::memcpy(LayerMap.data() + layerOffset, source + static_cast<size_t>(y) * sourcePitch, rect.size.width);
	}
	return true;
}

RenderLayerScope::RenderLayerScope(const RenderLayer layer)
    : layer_(layer)
{
	BeginRenderLayer(layer_);
}

RenderLayerScope::RenderLayerScope(const RenderLayer layer, const Rectangle captureBounds)
    : layer_(layer)
{
	BeginRenderLayer(layer_, captureBounds);
}

RenderLayerScope::~RenderLayerScope()
{
	EndRenderLayer(layer_);
}

RenderLayerCaptureSuspension::RenderLayerCaptureSuspension()
{
	CaptureSuspensionDepth++;
	RefreshCaptureActive();
}

RenderLayerCaptureSuspension::~RenderLayerCaptureSuspension()
{
	assert(CaptureSuspensionDepth > 0);
	CaptureSuspensionDepth--;
	RefreshCaptureActive();
}

} // namespace devilution
