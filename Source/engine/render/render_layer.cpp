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
#include <utility>
#include <vector>

#include "engine/surface.hpp"
#include "levels/dun_tile.hpp"

namespace devilution {
namespace {

struct CaptureBuffer {
	const void *surface = nullptr;
	const uint8_t *pixels = nullptr;
	int originX = 0;
	int originY = 0;
	int width = 0;
	int height = 0;
	int pitch = 0;
};

RenderLayerFrameStats FrameStats;
std::vector<RenderLayer> LayerStack;
struct WorldMaskState {
	RenderWorldMaterial material = RenderWorldMaterial::Unknown;
	uint8_t flags = 0;
};
std::vector<WorldMaskState> WorldMaskStack;
bool CaptureEnabled = false;
bool LayerMapCaptureEnabled = false;
bool WorldMaskCaptureEnabled = false;
bool WorldProxyCaptureEnabled = false;
bool WorldProxyActorOccludersEnabled = false;
int CaptureSuspensionDepth = 0;
CaptureBuffer ActiveBuffer;
CaptureBuffer LayerMapBuffer;
std::vector<uint8_t> LayerMap;
bool LayerMapDefaultsToWorld = false;
std::vector<Rectangle> LayerMapDirtyRects;
std::vector<Rectangle> LayerMapDefaultPreviousNonWorldRects;
std::vector<Rectangle> LayerMapDefaultCurrentNonWorldRects;
bool LayerMapDefaultsFinalized = false;
bool LayerMapDirtyFullFrame = false;
uint64_t NextLayerMapVersion = 0;
uint64_t CurrentLayerMapVersion = 0;
CaptureBuffer WorldMaskMapBuffer;
std::vector<uint8_t> WorldMaterialMap;
std::vector<uint8_t> WorldReceiverMap;
std::vector<uint8_t> WorldOccluderMap;
std::vector<uint8_t> WorldEmissiveMap;
uint64_t NextWorldMaskVersion = 0;
uint64_t CurrentWorldMaskVersion = 0;
CaptureBuffer WorldProxyMapBuffer;
std::vector<uint8_t> WorldProxyTypeMap;
std::vector<uint8_t> WorldProxyDepthMap;
std::vector<uint8_t> WorldProxyHeightMap;
std::vector<uint8_t> WorldProxyReceiverMap;
std::vector<uint8_t> WorldProxyOccluderMap;
uint64_t NextWorldProxyVersion = 0;
uint64_t CurrentWorldProxyVersion = 0;
bool ClassicLightCaptureEnabled = false;
bool ClassicLightFrameStamped = false;
bool ClassicLightGeneratedIntensityMap = false;
bool ClassicLightMapStoresIntensity = false;
uint8_t CurrentClassicLightLevel = FullyLitRenderClassicLightLevel;
std::vector<uint8_t> ClassicLightStack;
CaptureBuffer ClassicLightMapBuffer;
std::vector<uint8_t> ClassicLightMap;
bool ClassicLightGridEnabled = false;
bool ClassicLightGridActive = false;
std::vector<uint8_t> ClassicLightGrid;
Size ClassicLightGridSize;
int ClassicLightGridPitch = 0;
Point ClassicLightGridFirstTile;
Displacement ClassicLightGridOffset;
int ClassicLightGridViewportHeight = 0;
uint64_t NextClassicLightVersion = 0;
uint64_t CurrentClassicLightVersion = 0;
std::vector<RenderSmoothLightSource> SmoothLightSources;
uint64_t NextSmoothLightSourceVersion = 0;
uint64_t CurrentSmoothLightSourceVersion = 0;

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

[[nodiscard]] bool CanLocateCaptureSpan()
{
	return RenderLayerCaptureActive
	    && ActiveBuffer.pixels != nullptr
	    && ActiveBuffer.width > 0
	    && ActiveBuffer.height > 0;
}

[[nodiscard]] bool CanStampLayerMap()
{
	return LayerMapCaptureEnabled
	    && LayerMap.size() == LayerMapSize();
}

[[nodiscard]] bool CanAccessLayerMapRegion(const Rectangle &rect, const int pitch)
{
	return LayerMapCaptureEnabled
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

[[nodiscard]] Rectangle ClipLayerMapRect(Rectangle rect)
{
	if (ActiveBuffer.width <= 0 || ActiveBuffer.height <= 0)
		return {};

	const int x0 = std::clamp(rect.position.x, 0, ActiveBuffer.width);
	const int y0 = std::clamp(rect.position.y, 0, ActiveBuffer.height);
	const int x1 = std::clamp(rect.position.x + rect.size.width, 0, ActiveBuffer.width);
	const int y1 = std::clamp(rect.position.y + rect.size.height, 0, ActiveBuffer.height);
	return { { x0, y0 }, { std::max(x1 - x0, 0), std::max(y1 - y0, 0) } };
}

[[nodiscard]] bool IsEmpty(const Rectangle &rect)
{
	return rect.size.width <= 0 || rect.size.height <= 0;
}

void MarkLayerMapDirtyFullFrame()
{
	LayerMapDirtyRects.clear();
	LayerMapDirtyFullFrame = true;
	CurrentLayerMapVersion = ++NextLayerMapVersion;
}

void MarkLayerMapDirtyRect(Rectangle rect)
{
	if (LayerMapDirtyFullFrame)
		return;
	rect = ClipLayerMapRect(rect);
	if (IsEmpty(rect))
		return;
	LayerMapDirtyRects.push_back(rect);
	CurrentLayerMapVersion = ++NextLayerMapVersion;
}

[[nodiscard]] Rectangle LayerMapSpanRect(const size_t index, const int width)
{
	if (ActiveBuffer.width <= 0)
		return {};
	const int x = static_cast<int>(index % static_cast<size_t>(ActiveBuffer.width));
	const int y = static_cast<int>(index / static_cast<size_t>(ActiveBuffer.width));
	return { { x, y }, { width, 1 } };
}

void ResetLayerMapRectToWorld(Rectangle rect)
{
	if (!CanStampLayerMap())
		return;
	rect = ClipLayerMapRect(rect);
	if (IsEmpty(rect))
		return;

	for (int y = 0; y < rect.size.height; y++) {
		const size_t offset = static_cast<size_t>(rect.position.y + y) * ActiveBuffer.width + rect.position.x;
		std::memset(LayerMap.data() + offset, static_cast<uint8_t>(RenderLayer::World), static_cast<size_t>(rect.size.width));
	}
	MarkLayerMapDirtyRect(rect);
}

[[nodiscard]] bool LayerMapSpanContainsWorld(const size_t index, const int width)
{
	const uint8_t world = static_cast<uint8_t>(RenderLayer::World);
	const uint8_t *span = LayerMap.data() + index;
	return std::find(span, span + width, world) != span + width;
}

[[nodiscard]] bool LayerMapSpanContainsNonWorld(const size_t index, const int width)
{
	const uint8_t *span = LayerMap.data() + index;
	return std::find_if(span, span + width, [](const uint8_t value) { return value != static_cast<uint8_t>(RenderLayer::World); }) != span + width;
}

[[nodiscard]] std::vector<Rectangle> NormalizeLayerMapRowSpans(std::vector<Rectangle> spans)
{
	for (Rectangle &span : spans)
		span = ClipLayerMapRect(span);
	spans.erase(std::remove_if(spans.begin(), spans.end(), IsEmpty), spans.end());
	std::sort(spans.begin(), spans.end(), [](const Rectangle &a, const Rectangle &b) {
		if (a.position.y != b.position.y)
			return a.position.y < b.position.y;
		return a.position.x < b.position.x;
	});

	std::vector<Rectangle> merged;
	for (const Rectangle &span : spans) {
		if (merged.empty() || merged.back().position.y != span.position.y || span.position.x > merged.back().position.x + merged.back().size.width) {
			merged.push_back(span);
			continue;
		}
		const int mergedEnd = std::max(merged.back().position.x + merged.back().size.width, span.position.x + span.size.width);
		merged.back().size.width = mergedEnd - merged.back().position.x;
	}
	return merged;
}

[[nodiscard]] bool LayerMapRowSpansEqual(const std::vector<Rectangle> &a, const std::vector<Rectangle> &b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i].position != b[i].position || a[i].size != b[i].size)
			return false;
	}
	return true;
}

void FinalizeLayerMapDefaultToWorld()
{
	if (!LayerMapDefaultsToWorld || LayerMapDefaultsFinalized || !CanStampLayerMap())
		return;

	std::vector<Rectangle> current = NormalizeLayerMapRowSpans(std::move(LayerMapDefaultCurrentNonWorldRects));
	std::vector<Rectangle> previous = NormalizeLayerMapRowSpans(std::move(LayerMapDefaultPreviousNonWorldRects));
	LayerMapDefaultCurrentNonWorldRects.clear();
	LayerMapDefaultPreviousNonWorldRects.clear();

	if (LayerMapDirtyRects.empty() && !LayerMapDirtyFullFrame && LayerMapRowSpansEqual(current, previous)) {
		LayerMapDefaultPreviousNonWorldRects = std::move(current);
		LayerMapDefaultsFinalized = true;
		return;
	}

	size_t currentIndex = 0;
	for (const Rectangle &previousSpan : previous) {
		while (currentIndex < current.size() && current[currentIndex].position.y < previousSpan.position.y)
			currentIndex++;

		int uncoveredStart = previousSpan.position.x;
		const int previousEnd = previousSpan.position.x + previousSpan.size.width;
		for (size_t i = currentIndex; i < current.size() && current[i].position.y == previousSpan.position.y && current[i].position.x < previousEnd; i++) {
			const int currentStart = current[i].position.x;
			const int currentEnd = current[i].position.x + current[i].size.width;
			if (currentEnd <= uncoveredStart)
				continue;
			if (currentStart > uncoveredStart)
				ResetLayerMapRectToWorld({ { uncoveredStart, previousSpan.position.y }, { std::min(currentStart, previousEnd) - uncoveredStart, 1 } });
			uncoveredStart = std::max(uncoveredStart, currentEnd);
			if (uncoveredStart >= previousEnd)
				break;
		}
		if (uncoveredStart < previousEnd)
			ResetLayerMapRectToWorld({ { uncoveredStart, previousSpan.position.y }, { previousEnd - uncoveredStart, 1 } });
	}

	LayerMapDefaultPreviousNonWorldRects = std::move(current);
	LayerMapDefaultsFinalized = true;
}

void RefreshCaptureActive()
{
	RenderLayerCaptureActive = CaptureEnabled && CaptureSuspensionDepth == 0;
}

void MarkLayerMapSpan(const size_t index, const int width)
{
	if (LayerMapDefaultsToWorld) {
		const Rectangle span = LayerMapSpanRect(index, width);
		if (FrameStats.currentLayer != RenderLayer::World) {
			if (LayerMapSpanContainsWorld(index, width))
				MarkLayerMapDirtyRect(span);
			LayerMapDefaultCurrentNonWorldRects.push_back(span);
		} else if (LayerMapSpanContainsNonWorld(index, width)) {
			MarkLayerMapDirtyRect(span);
		}
	}
	std::memset(LayerMap.data() + index, static_cast<uint8_t>(FrameStats.currentLayer), static_cast<size_t>(width));
}

[[nodiscard]] bool ShouldStampWorldMask()
{
	return WorldMaskCaptureEnabled
	    && FrameStats.currentLayer == RenderLayer::World
	    && WorldMaterialMap.size() == LayerMapSize()
	    && WorldReceiverMap.size() == LayerMapSize()
	    && WorldOccluderMap.size() == LayerMapSize()
	    && WorldEmissiveMap.size() == LayerMapSize();
}

void MarkWorldMaskSpan(const size_t index, const int width)
{
	if (!ShouldStampWorldMask())
		return;

	const uint8_t material = static_cast<uint8_t>(CurrentRenderWorldMaterial());
	const uint8_t receiver = (CurrentRenderWorldMaskFlags() & RenderWorldMaskReceiver) != 0 ? 255 : 0;
	const uint8_t occluder = (CurrentRenderWorldMaskFlags() & RenderWorldMaskOccluder) != 0 ? 255 : 0;
	const uint8_t emissive = (CurrentRenderWorldMaskFlags() & RenderWorldMaskEmissive) != 0 ? 255 : 0;
	std::memset(WorldMaterialMap.data() + index, material, static_cast<size_t>(width));
	std::memset(WorldReceiverMap.data() + index, receiver, static_cast<size_t>(width));
	std::memset(WorldOccluderMap.data() + index, occluder, static_cast<size_t>(width));
	std::memset(WorldEmissiveMap.data() + index, emissive, static_cast<size_t>(width));
	FrameStats.worldMaskStampedSpanCount++;
	FrameStats.worldMaskStampedPixelCount += static_cast<uint64_t>(width);
}

[[nodiscard]] bool CanStampClassicLight()
{
	return ClassicLightCaptureEnabled
	    && ClassicLightMap.size() == LayerMapSize();
}

[[nodiscard]] uint8_t ClassicLightLevelForCurrentLayer()
{
	return FrameStats.currentLayer == RenderLayer::World ? CurrentClassicLightLevel : NonWorldRenderClassicLightLevel;
}

[[nodiscard]] uint8_t ClassicLightValueForCurrentLayer()
{
	if (ClassicLightGeneratedIntensityMap)
		return 255;
	return ClassicLightLevelForCurrentLayer();
}

void MarkClassicLightSpan(const size_t index, const int width)
{
	if (!CanStampClassicLight())
		return;
	if (ClassicLightGeneratedIntensityMap && FrameStats.currentLayer == RenderLayer::World)
		return;

	if (!ClassicLightFrameStamped) {
		CurrentClassicLightVersion = ++NextClassicLightVersion;
		ClassicLightFrameStamped = true;
	}
	std::memset(ClassicLightMap.data() + index, ClassicLightValueForCurrentLayer(), static_cast<size_t>(width));
	if (FrameStats.currentLayer == RenderLayer::World) {
		FrameStats.classicLightStampedSpanCount++;
		FrameStats.classicLightStampedPixelCount += static_cast<uint64_t>(width);
	}
}

[[nodiscard]] bool ShouldStampWorldProxy()
{
	return WorldProxyCaptureEnabled
	    && ActiveBuffer.width > 0
	    && ActiveBuffer.height > 0
	    && WorldProxyTypeMap.size() == LayerMapSize()
	    && WorldProxyDepthMap.size() == LayerMapSize()
	    && WorldProxyHeightMap.size() == LayerMapSize()
	    && WorldProxyReceiverMap.size() == LayerMapSize()
	    && WorldProxyOccluderMap.size() == LayerMapSize();
}

[[nodiscard]] uint8_t ProxyDepthAtY(const int y)
{
	if (ActiveBuffer.height <= 1)
		return 1;
	return static_cast<uint8_t>(std::clamp(1 + y * 254 / (ActiveBuffer.height - 1), 1, 255));
}

[[nodiscard]] uint8_t ProxyHeightForPrimitive(const RenderWorldProxyPrimitive primitive)
{
	switch (primitive) {
	case RenderWorldProxyPrimitive::FloorDiamond:
		return 32;
	case RenderWorldProxyPrimitive::ObjectBlocker:
		return 128;
	case RenderWorldProxyPrimitive::ActorBillboard:
		return 160;
	case RenderWorldProxyPrimitive::LeftWallQuad:
	case RenderWorldProxyPrimitive::RightWallQuad:
	case RenderWorldProxyPrimitive::DoorArchBlocker:
		return 192;
	case RenderWorldProxyPrimitive::Count:
		break;
	}
	return 0;
}

[[nodiscard]] bool ProxyPrimitiveReceivesLight(const RenderWorldProxyPrimitive primitive)
{
	switch (primitive) {
	case RenderWorldProxyPrimitive::FloorDiamond:
	case RenderWorldProxyPrimitive::LeftWallQuad:
	case RenderWorldProxyPrimitive::RightWallQuad:
	case RenderWorldProxyPrimitive::DoorArchBlocker:
	case RenderWorldProxyPrimitive::ObjectBlocker:
		return true;
	case RenderWorldProxyPrimitive::ActorBillboard:
	case RenderWorldProxyPrimitive::Count:
		return false;
	}
	return false;
}

[[nodiscard]] bool ProxyPrimitiveOccludesLight(const RenderWorldProxyPrimitive primitive)
{
	switch (primitive) {
	case RenderWorldProxyPrimitive::FloorDiamond:
		return false;
	case RenderWorldProxyPrimitive::LeftWallQuad:
	case RenderWorldProxyPrimitive::RightWallQuad:
	case RenderWorldProxyPrimitive::DoorArchBlocker:
	case RenderWorldProxyPrimitive::ObjectBlocker:
		return true;
	case RenderWorldProxyPrimitive::ActorBillboard:
		return WorldProxyActorOccludersEnabled;
	case RenderWorldProxyPrimitive::Count:
		break;
	}
	return false;
}

void StampWorldProxySpan(const int xBegin, const int y, const int width, const RenderWorldProxyPrimitive primitive)
{
	if (!ShouldStampWorldProxy() || y < 0 || y >= ActiveBuffer.height || width <= 0)
		return;

	int clippedX = xBegin;
	int clippedWidth = width;
	if (clippedX < 0) {
		clippedWidth += clippedX;
		clippedX = 0;
	}
	if (clippedX + clippedWidth > ActiveBuffer.width)
		clippedWidth = ActiveBuffer.width - clippedX;
	if (clippedWidth <= 0)
		return;

	const uint8_t depth = ProxyDepthAtY(y);
	const uint8_t height = ProxyHeightForPrimitive(primitive);
	const uint8_t receiver = ProxyPrimitiveReceivesLight(primitive) ? 255 : 0;
	const uint8_t occluder = ProxyPrimitiveOccludesLight(primitive) ? 255 : 0;
	const size_t index = static_cast<size_t>(y) * ActiveBuffer.width + clippedX;
	std::memset(WorldProxyTypeMap.data() + index, static_cast<uint8_t>(primitive), static_cast<size_t>(clippedWidth));
	std::memset(WorldProxyDepthMap.data() + index, depth, static_cast<size_t>(clippedWidth));
	std::memset(WorldProxyHeightMap.data() + index, height, static_cast<size_t>(clippedWidth));
	std::memset(WorldProxyReceiverMap.data() + index, receiver, static_cast<size_t>(clippedWidth));
	std::memset(WorldProxyOccluderMap.data() + index, occluder, static_cast<size_t>(clippedWidth));
	FrameStats.worldProxyPixelCount += static_cast<uint64_t>(clippedWidth);
}

void StampWorldProxyRectangle(Rectangle rect, const RenderWorldProxyPrimitive primitive)
{
	if (!ShouldStampWorldProxy() || rect.size.width <= 0 || rect.size.height <= 0)
		return;

	const int yBegin = std::clamp(rect.position.y, 0, ActiveBuffer.height);
	const int yEnd = std::clamp(rect.position.y + rect.size.height, 0, ActiveBuffer.height);
	for (int y = yBegin; y < yEnd; y++) {
		StampWorldProxySpan(rect.position.x, y, rect.size.width, primitive);
	}
}

[[nodiscard]] bool TryGetLayerMapSpan(const uint8_t *dst, int width, size_t &index, int &clippedWidth)
{
	if (!CanLocateCaptureSpan() || dst == nullptr || width <= 0)
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

[[nodiscard]] bool TryGetLayerMapSpan(const Surface &surface, Point position, int width, size_t &index, int &clippedWidth)
{
	if (!CanLocateCaptureSpan() || !ClipSurfaceSpan(surface, position, width) || surface.surface != ActiveBuffer.surface)
		return false;

	const int x = surface.region.x + position.x - ActiveBuffer.originX;
	const int y = surface.region.y + position.y - ActiveBuffer.originY;
	if (x < 0 || y < 0 || x >= ActiveBuffer.width || y >= ActiveBuffer.height)
		return false;

	clippedWidth = std::min(width, ActiveBuffer.width - x);
	if (clippedWidth <= 0)
		return false;

	index = static_cast<size_t>(y) * ActiveBuffer.width + x;
	return true;
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
	if (ClassicLightGeneratedIntensityMap && FrameStats.currentLayer == RenderLayer::World && !LayerMapCaptureEnabled && !WorldMaskCaptureEnabled && !WorldProxyCaptureEnabled)
		return;
	if (LayerMapDefaultsToWorld && FrameStats.currentLayer == RenderLayer::World && !WorldMaskCaptureEnabled && !WorldProxyCaptureEnabled && !ClassicLightCaptureEnabled)
		return;

	size_t index = 0;
	int clippedWidth = 0;
	if (!TryGetLayerMapSpan(dst, width, index, clippedWidth))
		return;

	if (CanStampLayerMap()) {
		FrameStats.stampedSpanCount++;
		FrameStats.stampedPixelCount += static_cast<uint64_t>(clippedWidth);
		MarkLayerMapSpan(index, clippedWidth);
	}
	MarkWorldMaskSpan(index, clippedWidth);
	MarkClassicLightSpan(index, clippedWidth);
}

void MarkRenderLayerPixelSlow(const Surface &surface, const Point position)
{
	MarkRenderLayerSpanSlow(surface, position, 1);
}

void MarkRenderLayerSpanSlow(const Surface &surface, Point position, int width)
{
	if (ClassicLightGeneratedIntensityMap && FrameStats.currentLayer == RenderLayer::World && !LayerMapCaptureEnabled && !WorldMaskCaptureEnabled && !WorldProxyCaptureEnabled)
		return;
	if (LayerMapDefaultsToWorld && FrameStats.currentLayer == RenderLayer::World && !WorldMaskCaptureEnabled && !WorldProxyCaptureEnabled && !ClassicLightCaptureEnabled)
		return;

	size_t index = 0;
	int clippedWidth = 0;
	if (!TryGetLayerMapSpan(surface, position, width, index, clippedWidth))
		return;

	if (CanStampLayerMap()) {
		FrameStats.stampedSpanCount++;
		FrameStats.stampedPixelCount += static_cast<uint64_t>(clippedWidth);
		MarkLayerMapSpan(index, clippedWidth);
	}
	MarkWorldMaskSpan(index, clippedWidth);
	MarkClassicLightSpan(index, clippedWidth);
}

void MarkRenderLayerRectSlow(const Surface &surface, Rectangle rect)
{
	if (!ClipSurfaceRect(surface, rect))
		return;

	for (int y = 0; y < rect.size.height; y++) {
		MarkRenderLayerSpanSlow(surface, { rect.position.x, rect.position.y + y }, rect.size.width);
	}
}

void MarkRenderLayerRect(const RenderLayer layer, Rectangle rect)
{
	if (!CanStampLayerMap())
		return;

	rect = ClipLayerMapRect(rect);
	if (IsEmpty(rect))
		return;

	const RenderLayer previousLayer = FrameStats.currentLayer;
	FrameStats.currentLayer = layer;
	for (int y = 0; y < rect.size.height; y++) {
		const size_t index = static_cast<size_t>(rect.position.y + y) * static_cast<size_t>(ActiveBuffer.width) + static_cast<size_t>(rect.position.x);
		MarkLayerMapSpan(index, rect.size.width);
	}
	FrameStats.currentLayer = previousLayer;
}

RenderLayer CurrentRenderLayer()
{
	return FrameStats.currentLayer;
}

RenderWorldMaterial CurrentRenderWorldMaterial()
{
	if (WorldMaskStack.empty())
		return RenderWorldMaterial::Unknown;
	return WorldMaskStack.back().material;
}

uint8_t CurrentRenderWorldMaskFlags()
{
	if (WorldMaskStack.empty())
		return 0;
	return WorldMaskStack.back().flags;
}

const RenderLayerFrameStats &GetRenderLayerFrameStats()
{
	return FrameStats;
}

RenderLayerMapView CurrentRenderLayerMapView()
{
	if (!LayerMapCaptureEnabled || LayerMap.empty())
		return {};

	FinalizeLayerMapDefaultToWorld();
	return {
		LayerMap.data(),
		ActiveBuffer.width,
		ActiveBuffer.height,
		ActiveBuffer.width,
		LayerMapDirtyRects.data(),
		LayerMapDirtyRects.size(),
		LayerMapDirtyFullFrame,
		CurrentLayerMapVersion,
	};
}

RenderWorldMaskMapView CurrentRenderWorldMaskMapView()
{
	if (!WorldMaskCaptureEnabled || WorldMaterialMap.empty())
		return {};

	return {
		WorldMaterialMap.data(),
		WorldReceiverMap.data(),
		WorldOccluderMap.data(),
		WorldEmissiveMap.data(),
		ActiveBuffer.width,
		ActiveBuffer.height,
		ActiveBuffer.width,
		CurrentWorldMaskVersion,
	};
}

RenderWorldProxyMapView CurrentRenderWorldProxyMapView()
{
	if (!WorldProxyCaptureEnabled || WorldProxyTypeMap.empty() || WorldProxyDepthMap.empty())
		return {};

	return {
		WorldProxyTypeMap.data(),
		WorldProxyDepthMap.data(),
		WorldProxyHeightMap.data(),
		WorldProxyReceiverMap.data(),
		WorldProxyOccluderMap.data(),
		ActiveBuffer.width,
		ActiveBuffer.height,
		ActiveBuffer.width,
		CurrentWorldProxyVersion,
	};
}

RenderClassicLightMapView CurrentRenderClassicLightMapView()
{
	if (ClassicLightGridEnabled && ClassicLightGridActive && !ClassicLightGrid.empty())
		return {
			ClassicLightGrid.data(),
			ClassicLightGridSize.width,
			ClassicLightGridSize.height,
			ClassicLightGridPitch,
			CurrentClassicLightVersion,
			false,
			true,
			ClassicLightGridFirstTile,
			ClassicLightGridOffset,
			ClassicLightGridViewportHeight,
		};
	if (!ClassicLightCaptureEnabled || ClassicLightMap.empty())
		return {};

	return {
		ClassicLightMap.data(),
		ActiveBuffer.width,
		ActiveBuffer.height,
		ActiveBuffer.width,
		CurrentClassicLightVersion,
		ClassicLightMapStoresIntensity,
		false,
		{},
		{},
		0,
	};
}

MutableRenderClassicLightMapView CurrentMutableRenderClassicLightMapView()
{
	if (!ClassicLightCaptureEnabled || ClassicLightMap.empty())
		return {};

	return {
		ClassicLightMap.data(),
		ActiveBuffer.width,
		ActiveBuffer.height,
		ActiveBuffer.width,
		ClassicLightMapStoresIntensity,
	};
}

RenderSmoothLightSourceView CurrentRenderSmoothLightSourceView()
{
	if (SmoothLightSources.empty())
		return {};

	return {
		SmoothLightSources.data(),
		SmoothLightSources.size(),
		CurrentSmoothLightSourceVersion,
	};
}

void SetRenderClassicLightGrid(const uint8_t *pixels, const Size size, const int pitch, const Point firstTile, const Displacement offset, const int viewportHeight)
{
	if (!ClassicLightGridEnabled || pixels == nullptr || size.width <= 0 || size.height <= 0 || pitch < size.width || viewportHeight <= 0)
		return;

	const size_t gridSize = static_cast<size_t>(size.width) * static_cast<size_t>(size.height);
	if (ClassicLightGrid.size() != gridSize)
		ClassicLightGrid.resize(gridSize);

	for (int y = 0; y < size.height; y++) {
		std::memcpy(
		    ClassicLightGrid.data() + static_cast<size_t>(y) * static_cast<size_t>(size.width),
		    pixels + static_cast<size_t>(y) * static_cast<size_t>(pitch),
		    static_cast<size_t>(size.width));
	}
	ClassicLightGridSize = size;
	ClassicLightGridPitch = size.width;
	ClassicLightGridFirstTile = firstTile;
	ClassicLightGridOffset = offset;
	ClassicLightGridViewportHeight = viewportHeight;
	ClassicLightGridActive = true;
	CurrentClassicLightVersion = ++NextClassicLightVersion;
}

void SetRenderSmoothLightSources(std::vector<RenderSmoothLightSource> sources)
{
	SmoothLightSources = std::move(sources);
	CurrentSmoothLightSourceVersion = SmoothLightSources.empty() ? 0 : ++NextSmoothLightSourceVersion;
	FrameStats.smoothLightSourceCount = SmoothLightSources.size();
}

void MarkRenderClassicLightMapChanged(const bool storesIntensity)
{
	if (!ClassicLightCaptureEnabled || ClassicLightMap.empty())
		return;
	ClassicLightMapStoresIntensity = storesIntensity;
	CurrentClassicLightVersion = ++NextClassicLightVersion;
	ClassicLightFrameStamped = true;
}

void ResetRenderLayerFrameStats()
{
	FrameStats = {};
	LayerStack.clear();
	WorldMaskStack.clear();
	ClassicLightStack.clear();
	CaptureEnabled = false;
	LayerMapCaptureEnabled = false;
	WorldMaskCaptureEnabled = false;
	WorldProxyCaptureEnabled = false;
	WorldProxyActorOccludersEnabled = false;
	ClassicLightCaptureEnabled = false;
	ClassicLightFrameStamped = false;
	ClassicLightGeneratedIntensityMap = false;
	ClassicLightMapStoresIntensity = false;
	CurrentClassicLightLevel = FullyLitRenderClassicLightLevel;
	CaptureSuspensionDepth = 0;
	RefreshCaptureActive();
	ActiveBuffer = {};
	LayerMapBuffer = {};
	LayerMap.clear();
	LayerMapDefaultsToWorld = false;
	LayerMapDirtyRects.clear();
	LayerMapDefaultPreviousNonWorldRects.clear();
	LayerMapDefaultCurrentNonWorldRects.clear();
	LayerMapDefaultsFinalized = false;
	LayerMapDirtyFullFrame = false;
	CurrentLayerMapVersion = 0;
	WorldMaskMapBuffer = {};
	WorldMaterialMap.clear();
	WorldReceiverMap.clear();
	WorldOccluderMap.clear();
	WorldEmissiveMap.clear();
	CurrentWorldMaskVersion = 0;
	WorldProxyMapBuffer = {};
	WorldProxyTypeMap.clear();
	WorldProxyDepthMap.clear();
	WorldProxyHeightMap.clear();
	WorldProxyReceiverMap.clear();
	WorldProxyOccluderMap.clear();
	CurrentWorldProxyVersion = 0;
	ClassicLightMapBuffer = {};
	ClassicLightMap.clear();
	ClassicLightGridEnabled = false;
	ClassicLightGridActive = false;
	ClassicLightGrid.clear();
	ClassicLightGridSize = {};
	ClassicLightGridPitch = 0;
	ClassicLightGridFirstTile = {};
	ClassicLightGridOffset = {};
	ClassicLightGridViewportHeight = 0;
	CurrentClassicLightVersion = 0;
	SmoothLightSources.clear();
	CurrentSmoothLightSourceVersion = 0;
}

void BeginRenderLayerFrame(const Surface &surface, const bool captureEnabled, const bool worldMaskCaptureEnabled, const bool worldProxyCaptureEnabled, const bool worldProxyActorOccludersEnabled, const bool classicLightCaptureEnabled, const bool classicLightGeneratedIntensityMap, const bool classicLightGridEnabled, const bool layerMapDefaultsToWorld)
{
	FrameStats = {};
	LayerStack.clear();
	WorldMaskStack.clear();
	ClassicLightStack.clear();
	LayerMapDirtyRects.clear();
	LayerMapDefaultCurrentNonWorldRects.clear();
	LayerMapDefaultsFinalized = false;
	LayerMapDirtyFullFrame = false;
	SmoothLightSources.clear();
	CurrentSmoothLightSourceVersion = 0;
	CurrentClassicLightLevel = FullyLitRenderClassicLightLevel;
	ClassicLightFrameStamped = false;
	ClassicLightMapStoresIntensity = false;
	ClassicLightGridEnabled = classicLightGridEnabled;
	if (!ClassicLightGridEnabled) {
		ClassicLightGridActive = false;
		ClassicLightGrid.clear();
		ClassicLightGridSize = {};
		ClassicLightGridPitch = 0;
		ClassicLightGridFirstTile = {};
		ClassicLightGridOffset = {};
		ClassicLightGridViewportHeight = 0;
	}
	CaptureSuspensionDepth = 0;
	const bool validSurface = surface.surface != nullptr && surface.w() > 0 && surface.h() > 0;
	LayerMapCaptureEnabled = captureEnabled && validSurface;
	LayerMapDefaultsToWorld = LayerMapCaptureEnabled && layerMapDefaultsToWorld;
	WorldMaskCaptureEnabled = worldMaskCaptureEnabled && validSurface;
	WorldProxyCaptureEnabled = worldProxyCaptureEnabled && validSurface;
	WorldProxyActorOccludersEnabled = worldProxyActorOccludersEnabled;
	ClassicLightCaptureEnabled = classicLightCaptureEnabled && validSurface;
	ClassicLightGeneratedIntensityMap = ClassicLightCaptureEnabled && classicLightGeneratedIntensityMap;
	ClassicLightMapStoresIntensity = ClassicLightGeneratedIntensityMap;
	CaptureEnabled = (LayerMapCaptureEnabled || WorldMaskCaptureEnabled || WorldProxyCaptureEnabled || ClassicLightCaptureEnabled) && validSurface;
	RefreshCaptureActive();
	if (!CaptureEnabled) {
		ActiveBuffer = {};
		LayerMapBuffer = {};
		LayerMap.clear();
		LayerMapDefaultsToWorld = false;
		LayerMapDirtyRects.clear();
		LayerMapDefaultPreviousNonWorldRects.clear();
		LayerMapDefaultCurrentNonWorldRects.clear();
		LayerMapDefaultsFinalized = false;
		LayerMapDirtyFullFrame = false;
		CurrentLayerMapVersion = 0;
		WorldMaskMapBuffer = {};
		WorldMaterialMap.clear();
		WorldReceiverMap.clear();
		WorldOccluderMap.clear();
		WorldEmissiveMap.clear();
		WorldProxyMapBuffer = {};
		WorldProxyTypeMap.clear();
		WorldProxyDepthMap.clear();
		WorldProxyHeightMap.clear();
		WorldProxyReceiverMap.clear();
		WorldProxyOccluderMap.clear();
		ClassicLightMapBuffer = {};
		ClassicLightMap.clear();
		ClassicLightGeneratedIntensityMap = false;
		ClassicLightMapStoresIntensity = false;
		return;
	}

	ActiveBuffer = {
		surface.surface,
		surface.begin(),
		surface.region.x,
		surface.region.y,
		surface.w(),
		surface.h(),
		surface.pitch(),
	};

	const size_t layerMapSize = LayerMapSize();
	if (!LayerMapCaptureEnabled) {
		LayerMapBuffer = {};
		LayerMap.clear();
		LayerMapDefaultPreviousNonWorldRects.clear();
		LayerMapDefaultCurrentNonWorldRects.clear();
		LayerMapDefaultsFinalized = false;
		CurrentLayerMapVersion = 0;
	} else if (!SameCaptureBuffer(ActiveBuffer, LayerMapBuffer) || LayerMap.size() != layerMapSize) {
		LayerMap.assign(layerMapSize, LayerMapDefaultsToWorld ? static_cast<uint8_t>(RenderLayer::World) : UnknownRenderLayerId);
		LayerMapBuffer = ActiveBuffer;
		LayerMapDefaultPreviousNonWorldRects.clear();
		LayerMapDefaultCurrentNonWorldRects.clear();
		LayerMapDefaultsFinalized = false;
		MarkLayerMapDirtyFullFrame();
	} else if (LayerMapDefaultsToWorld) {
		LayerMapDefaultsFinalized = false;
	} else {
		LayerMapDefaultPreviousNonWorldRects.clear();
		LayerMapDefaultCurrentNonWorldRects.clear();
		LayerMapDefaultsFinalized = false;
	}
	if (!WorldMaskCaptureEnabled) {
		WorldMaskMapBuffer = {};
		WorldMaterialMap.clear();
		WorldReceiverMap.clear();
		WorldOccluderMap.clear();
		WorldEmissiveMap.clear();
	}
	if (WorldMaskCaptureEnabled) {
		CurrentWorldMaskVersion = ++NextWorldMaskVersion;
		if (!SameCaptureBuffer(ActiveBuffer, WorldMaskMapBuffer) || WorldMaterialMap.size() != layerMapSize) {
			WorldMaterialMap.assign(layerMapSize, UnknownRenderWorldMaterialId);
			WorldReceiverMap.assign(layerMapSize, 0);
			WorldOccluderMap.assign(layerMapSize, 0);
			WorldEmissiveMap.assign(layerMapSize, 0);
			WorldMaskMapBuffer = ActiveBuffer;
		}
	}

	if (!ClassicLightCaptureEnabled) {
		ClassicLightMapBuffer = {};
		ClassicLightMap.clear();
	} else if (!SameCaptureBuffer(ActiveBuffer, ClassicLightMapBuffer) || ClassicLightMap.size() != layerMapSize) {
		ClassicLightMap.assign(layerMapSize, ClassicLightGeneratedIntensityMap ? 255 : NonWorldRenderClassicLightLevel);
		ClassicLightMapBuffer = ActiveBuffer;
		CurrentClassicLightVersion = ++NextClassicLightVersion;
		ClassicLightFrameStamped = true;
	} else {
		std::fill(ClassicLightMap.begin(), ClassicLightMap.end(), ClassicLightGeneratedIntensityMap ? 255 : NonWorldRenderClassicLightLevel);
		CurrentClassicLightVersion = ++NextClassicLightVersion;
		ClassicLightFrameStamped = true;
	}

	if (!WorldProxyCaptureEnabled) {
		WorldProxyMapBuffer = {};
		WorldProxyTypeMap.clear();
		WorldProxyDepthMap.clear();
		WorldProxyHeightMap.clear();
		WorldProxyReceiverMap.clear();
		WorldProxyOccluderMap.clear();
		return;
	}

	CurrentWorldProxyVersion = ++NextWorldProxyVersion;
	if (!SameCaptureBuffer(ActiveBuffer, WorldProxyMapBuffer) || WorldProxyDepthMap.size() != layerMapSize) {
		WorldProxyTypeMap.assign(layerMapSize, UnknownRenderWorldProxyPrimitiveId);
		WorldProxyDepthMap.assign(layerMapSize, 0);
		WorldProxyHeightMap.assign(layerMapSize, 0);
		WorldProxyReceiverMap.assign(layerMapSize, 0);
		WorldProxyOccluderMap.assign(layerMapSize, 0);
		WorldProxyMapBuffer = ActiveBuffer;
	} else {
		std::fill(WorldProxyTypeMap.begin(), WorldProxyTypeMap.end(), UnknownRenderWorldProxyPrimitiveId);
		std::fill(WorldProxyDepthMap.begin(), WorldProxyDepthMap.end(), 0);
		std::fill(WorldProxyHeightMap.begin(), WorldProxyHeightMap.end(), 0);
		std::fill(WorldProxyReceiverMap.begin(), WorldProxyReceiverMap.end(), 0);
		std::fill(WorldProxyOccluderMap.begin(), WorldProxyOccluderMap.end(), 0);
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

void BeginRenderWorldMask(const RenderWorldMaterial material, const uint8_t flags)
{
	WorldMaskStack.push_back({ material, flags });
}

void EndRenderWorldMask(const RenderWorldMaterial material)
{
	assert(!WorldMaskStack.empty());
	assert(WorldMaskStack.back().material == material);
	WorldMaskStack.pop_back();
}

void BeginRenderClassicLight(const uint8_t lightLevel)
{
	ClassicLightStack.push_back(CurrentClassicLightLevel);
	CurrentClassicLightLevel = lightLevel;
}

void EndRenderClassicLight(const uint8_t lightLevel)
{
	assert(CurrentClassicLightLevel == lightLevel);
	if (ClassicLightStack.empty()) {
		CurrentClassicLightLevel = FullyLitRenderClassicLightLevel;
		return;
	}

	CurrentClassicLightLevel = ClassicLightStack.back();
	ClassicLightStack.pop_back();
}

void MarkRenderWorldProxyPrimitive(const RenderWorldProxyPrimitive primitive, const Rectangle bounds)
{
	if (!ShouldStampWorldProxy())
		return;
	if (primitive == RenderWorldProxyPrimitive::ActorBillboard && !WorldProxyActorOccludersEnabled)
		return;

	FrameStats.worldProxyPrimitiveCount++;
	if (primitive == RenderWorldProxyPrimitive::ActorBillboard)
		FrameStats.worldProxyActorPrimitiveCount++;
	StampWorldProxyRectangle(bounds, primitive);
}

void MarkRenderWorldProxyFloorDiamond(const Point position)
{
	if (!ShouldStampWorldProxy())
		return;

	FrameStats.worldProxyPrimitiveCount++;
	const int top = position.y - TILE_HEIGHT + 1;
	const int centerX = position.x + TILE_WIDTH / 2;
	for (int row = 0; row < TILE_HEIGHT; row++) {
		const int rowWidth = row < TILE_HEIGHT / 2 ? (row + 1) * 4 : (TILE_HEIGHT - row) * 4;
		StampWorldProxySpan(centerX - rowWidth / 2, top + row, rowWidth, RenderWorldProxyPrimitive::FloorDiamond);
	}
}

void MarkRenderWorldProxyTilePrimitive(const RenderWorldProxyPrimitive primitive, const Point position)
{
	MarkRenderWorldProxyPrimitive(primitive, { { position.x, position.y - TILE_HEIGHT + 1 }, { DunFrameWidth, TILE_HEIGHT } });
}

void MarkRenderWorldProxyActorBillboard(const Rectangle bounds)
{
	MarkRenderWorldProxyPrimitive(RenderWorldProxyPrimitive::ActorBillboard, bounds);
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

	bool neutralChanged = false;
	const uint8_t world = static_cast<uint8_t>(RenderLayer::World);
	if (LayerMapDefaultsToWorld) {
		for (int y = 0; y < rect.size.height && !neutralChanged; y++) {
			const size_t layerOffset = static_cast<size_t>(rect.position.y + y) * ActiveBuffer.width + rect.position.x;
			const uint8_t *destinationRow = LayerMap.data() + layerOffset;
			const uint8_t *sourceRow = source + static_cast<size_t>(y) * sourcePitch;
			for (int x = 0; x < rect.size.width; x++) {
				if ((destinationRow[x] == world) != (sourceRow[x] == world)) {
					neutralChanged = true;
					break;
				}
			}
		}
	}

	for (int y = 0; y < rect.size.height; y++) {
		const size_t layerOffset = static_cast<size_t>(rect.position.y + y) * ActiveBuffer.width + rect.position.x;
		std::memcpy(LayerMap.data() + layerOffset, source + static_cast<size_t>(y) * sourcePitch, rect.size.width);
	}
	if (LayerMapDefaultsToWorld) {
		if (neutralChanged)
			MarkLayerMapDirtyRect(rect);
		for (int y = 0; y < rect.size.height; y++) {
			const uint8_t *sourceRow = source + static_cast<size_t>(y) * sourcePitch;
			int runStart = -1;
			for (int x = 0; x < rect.size.width; x++) {
				if (sourceRow[x] != world) {
					if (runStart < 0)
						runStart = x;
				} else if (runStart >= 0) {
					LayerMapDefaultCurrentNonWorldRects.push_back({ { rect.position.x + runStart, rect.position.y + y }, { x - runStart, 1 } });
					runStart = -1;
				}
			}
			if (runStart >= 0)
				LayerMapDefaultCurrentNonWorldRects.push_back({ { rect.position.x + runStart, rect.position.y + y }, { rect.size.width - runStart, 1 } });
		}
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

RenderWorldMaskScope::RenderWorldMaskScope(const RenderWorldMaterial material, const uint8_t flags)
    : material_(material)
{
	BeginRenderWorldMask(material_, flags);
}

RenderWorldMaskScope::~RenderWorldMaskScope()
{
	EndRenderWorldMask(material_);
}

RenderClassicLightScope::RenderClassicLightScope(const uint8_t lightLevel)
    : lightLevel_(lightLevel)
{
	BeginRenderClassicLight(lightLevel_);
}

RenderClassicLightScope::~RenderClassicLightScope()
{
	EndRenderClassicLight(lightLevel_);
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
