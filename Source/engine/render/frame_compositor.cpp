/**
 * @file frame_compositor.cpp
 *
 * Final frame composition helpers for the indexed renderer.
 */
#include "engine/render/frame_compositor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

#if defined(WINVER) && WINVER <= 0x0500 && (!defined(_WIN32_WINNT) || _WIN32_WINNT == 0)
#define DEVILUTIONX_LEGACY_WINDOWS_9X 1
#else
#define DEVILUTIONX_LEGACY_WINDOWS_9X 0
#endif

#if defined(__DJGPP__) || defined(__EMSCRIPTEN__) || defined(__amigaos__) || defined(__UWP__) \
    || defined(__3DS__) || defined(__SWITCH__) || defined(__vita__) || defined(__ORBIS__)     \
    || defined(__PROSPERO__) || defined(NXDK) || DEVILUTIONX_LEGACY_WINDOWS_9X
#define DEVILUTIONX_PARALLEL_COMPOSITION 0
#else
#define DEVILUTIONX_PARALLEL_COMPOSITION 1
#endif

#if DEVILUTIONX_PARALLEL_COMPOSITION
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#endif

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "appfat.h"
#include "engine/dx.h"
#include "engine/palette.h"
#include "engine/render/accelerated_compositor_lifecycle.hpp"
#include "engine/render/render_layer.hpp"
#include "options.h"
#include "utils/display.h"
#include "utils/sdl_compat.h"

namespace devilution {
namespace {

CpuPaletteCompositor FrameCompositor;
constexpr size_t MaxDirtyRectsBeforeFullFrame = 64;
constexpr size_t RenderLayerDiagnosticColorCount = RenderLayerCount + 1;
constexpr size_t UnknownRenderLayerDiagnosticColorIndex = RenderLayerCount;
constexpr size_t RenderWorldMaterialColorCount = static_cast<size_t>(RenderWorldMaterial::Count);
constexpr size_t RenderWorldProxyPrimitiveColorCount = static_cast<size_t>(RenderWorldProxyPrimitive::Count) + 1;
#if DEVILUTIONX_PARALLEL_COMPOSITION
constexpr int MinParallelCompositionPixels = 96 * 1024;
constexpr int MinParallelCompositionRowsPerThread = 64;
constexpr unsigned MaxParallelCompositionThreads = 6;
#endif
#ifdef BUILD_TESTING
int ThreadCountOverrideForTesting = 0;
#endif

[[nodiscard]] bool UsesDirectPresentation(const FrameCompositorBackendResult result)
{
	return result == FrameCompositorBackendResult::PreparedDirectPresentation
	    || result == FrameCompositorBackendResult::RetainedDirectPresentation;
}

void RecordBackendResult(RenderPerfCompositionStats &stats, const FrameCompositorBackendResult result)
{
	switch (result) {
	case FrameCompositorBackendResult::NoFrameProduced:
		stats.backendNoFrameProducedCount++;
		break;
	case FrameCompositorBackendResult::UpdatedOutputSurface:
		stats.backendUpdatedOutputSurfaceCount++;
		break;
	case FrameCompositorBackendResult::PreparedDirectPresentation:
		stats.backendPreparedDirectPresentationCount++;
		break;
	case FrameCompositorBackendResult::RetainedDirectPresentation:
		stats.backendRetainedDirectPresentationCount++;
		break;
	}
}

#if DEVILUTIONX_PARALLEL_COMPOSITION
struct CompositionWorkerBand {
	int yBegin = 0;
	int yEnd = 0;
};

class CompositionWorkerPool {
public:
	~CompositionWorkerPool()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
			generation_++;
		}
		jobAvailable_.notify_all();
		for (std::thread &worker : workers_) {
			if (worker.joinable())
				worker.join();
		}
	}

	void Run(const int threadCount, const int yBegin, const int yEnd, const std::function<void(int, int)> &composeRows)
	{
		const size_t workerCount = static_cast<size_t>(threadCount - 1);
		int nextYBegin = yBegin;
		int mainYBegin = yBegin;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			EnsureWorkerCount(workerCount);
			activeWorkerCount_ = workerCount;
			remainingWorkerCount_ = workerCount;
			job_ = &composeRows;

			const int rowCount = yEnd - yBegin;
			const int rowsPerThread = rowCount / threadCount;
			const int extraRows = rowCount % threadCount;
			for (size_t i = 0; i < workerCount; i++) {
				const int bandHeight = rowsPerThread + (static_cast<int>(i) < extraRows ? 1 : 0);
				bands_[i] = { nextYBegin, nextYBegin + bandHeight };
				nextYBegin += bandHeight;
			}
			mainYBegin = nextYBegin;
			generation_++;
		}

		jobAvailable_.notify_all();
		composeRows(mainYBegin, yEnd);

		std::unique_lock<std::mutex> lock(mutex_);
		jobFinished_.wait(lock, [this]() { return remainingWorkerCount_ == 0; });
		job_ = nullptr;
		activeWorkerCount_ = 0;
	}

private:
	void EnsureWorkerCount(const size_t workerCount)
	{
		while (workers_.size() < workerCount) {
			const size_t workerIndex = workers_.size();
			workers_.emplace_back([this, workerIndex]() { WorkerLoop(workerIndex); });
		}
	}

	void WorkerLoop(const size_t workerIndex)
	{
		uint64_t observedGeneration = 0;
		std::unique_lock<std::mutex> lock(mutex_);
		while (true) {
			jobAvailable_.wait(lock, [this, &observedGeneration]() { return stopping_ || generation_ != observedGeneration; });
			if (stopping_)
				return;

			observedGeneration = generation_;
			if (workerIndex >= activeWorkerCount_)
				continue;

			const CompositionWorkerBand band = bands_[workerIndex];
			const std::function<void(int, int)> *job = job_;
			lock.unlock();
			(*job)(band.yBegin, band.yEnd);
			lock.lock();

			if (--remainingWorkerCount_ == 0)
				jobFinished_.notify_one();
		}
	}

	std::mutex mutex_;
	std::condition_variable jobAvailable_;
	std::condition_variable jobFinished_;
	std::vector<std::thread> workers_;
	std::array<CompositionWorkerBand, MaxParallelCompositionThreads - 1> bands_ {};
	const std::function<void(int, int)> *job_ = nullptr;
	uint64_t generation_ = 0;
	size_t activeWorkerCount_ = 0;
	size_t remainingWorkerCount_ = 0;
	bool stopping_ = false;
};

CompositionWorkerPool &CompositionWorkers()
{
	static CompositionWorkerPool workerPool;
	return workerPool;
}
#endif

struct NormalizedDirtyRects {
	DirtyRectList dirtyRects;
	int submittedRectCount = 0;
	int normalizedRectCount = 0;
	uint64_t submittedArea = 0;
	uint64_t normalizedArea = 0;
	bool tooManyDirtyRects = false;
};

[[nodiscard]] uint64_t Area(const Rectangle &rect)
{
	if (rect.size.width <= 0 || rect.size.height <= 0)
		return 0;
	return static_cast<uint64_t>(rect.size.width) * static_cast<uint64_t>(rect.size.height);
}

[[nodiscard]] uint64_t Area(const Size size)
{
	if (size.width <= 0 || size.height <= 0)
		return 0;
	return static_cast<uint64_t>(size.width) * static_cast<uint64_t>(size.height);
}

[[nodiscard]] int CompositionAttachmentBytesPerPixel(const CompositionAttachmentFormat format)
{
	switch (format) {
	case CompositionAttachmentFormat::Index8:
	case CompositionAttachmentFormat::Alpha8:
		return 1;
	case CompositionAttachmentFormat::PaletteRgba8:
	case CompositionAttachmentFormat::Rgba8:
		return 4;
	case CompositionAttachmentFormat::Unknown:
		break;
	}
	return 0;
}

void UpsertCompositionAttachment(std::vector<CompositionAttachment> &attachments, CompositionAttachment attachment)
{
	for (CompositionAttachment &existing : attachments) {
		if (existing.role == attachment.role) {
			existing = std::move(attachment);
			return;
		}
	}
	attachments.push_back(std::move(attachment));
}

[[nodiscard]] size_t CompositionSurfaceRoleIndex(const CompositionSurfaceRole role)
{
	return static_cast<size_t>(role);
}

[[nodiscard]] int BytesPerPixel(const SDL_Surface &surface)
{
#ifdef USE_SDL3
	return SDL_BYTESPERPIXEL(surface.format);
#else
	return surface.format->BytesPerPixel;
#endif
}

[[nodiscard]] uintptr_t OutputSurfaceFormatIdentity(const SDL_Surface &surface)
{
#ifdef USE_SDL3
	return static_cast<uintptr_t>(surface.format);
#else
	return reinterpret_cast<uintptr_t>(surface.format);
#endif
}

[[nodiscard]] uint32_t MapRgba(const SDL_Surface &surface, const RgbColor color)
{
#ifdef USE_SDL3
	return SDL_MapRGBA(SDL_GetPixelFormatDetails(surface.format), SDL_GetSurfacePalette(const_cast<SDL_Surface *>(&surface)), color.r, color.g, color.b, color.a);
#else
	return SDL_MapRGBA(surface.format, color.r, color.g, color.b, color.a);
#endif
}

[[nodiscard]] RgbColor ApplyDiagnosticTransform(RgbColor color)
{
	const uint8_t luma = static_cast<uint8_t>((static_cast<uint16_t>(color.r) * 30 + static_cast<uint16_t>(color.g) * 59 + static_cast<uint16_t>(color.b) * 11) / 100);
	color.r = std::min<int>(255, luma + 48);
	color.g = luma / 2;
	color.b = 255 - (luma / 3);
	return color;
}

[[nodiscard]] bool UsesRenderLayerTint(const RenderLayerDiagnosticMode mode)
{
	return mode == RenderLayerDiagnosticMode::Tint || mode == RenderLayerDiagnosticMode::TintAndOutline;
}

[[nodiscard]] bool UsesRenderLayerOutline(const RenderLayerDiagnosticMode mode)
{
	return mode == RenderLayerDiagnosticMode::Outline || mode == RenderLayerDiagnosticMode::TintAndOutline;
}

[[nodiscard]] bool WorldMaskDiagnosticEnabled(const CompositionFrame &frame)
{
	return frame.renderWorldMaskDiagnosticMode != RenderWorldMaskDiagnosticMode::Off
	    && frame.worldMaskMap.materialPixels != nullptr
	    && frame.worldMaskMap.width > 0
	    && frame.worldMaskMap.height > 0
	    && frame.worldMaskMap.pitch >= frame.worldMaskMap.width;
}

[[nodiscard]] bool WorldProxyDiagnosticEnabled(const CompositionFrame &frame)
{
	return frame.renderWorldProxyDiagnosticMode != RenderWorldProxyDiagnosticMode::Off
	    && frame.worldProxyMap.typePixels != nullptr
	    && frame.worldProxyMap.depthPixels != nullptr
	    && frame.worldProxyMap.heightPixels != nullptr
	    && frame.worldProxyMap.receiverPixels != nullptr
	    && frame.worldProxyMap.occluderPixels != nullptr
	    && frame.worldProxyMap.width > 0
	    && frame.worldProxyMap.height > 0
	    && frame.worldProxyMap.pitch >= frame.worldProxyMap.width;
}

[[nodiscard]] bool IsWorldLayerPixel(const RenderLayerMapView &layerMap, const int x, const int y)
{
	if (layerMap.pixels == nullptr || x < 0 || y < 0 || x >= layerMap.width || y >= layerMap.height)
		return true;
	const uint8_t layerId = layerMap.pixels[static_cast<size_t>(y) * layerMap.pitch + x];
	return layerId == static_cast<uint8_t>(RenderLayer::World);
}

[[nodiscard]] RgbColor RenderLayerDiagnosticColor(const uint8_t layerId)
{
	switch (static_cast<RenderLayer>(layerId)) {
	case RenderLayer::World:
		return { 0, 255, 0 };
	case RenderLayer::WorldOverlay:
		return { 255, 255, 0 };
	case RenderLayer::Interface:
		return { 0, 96, 255 };
	case RenderLayer::Cursor:
		return { 255, 0, 255 };
	case RenderLayer::Debug:
		return { 255, 0, 0 };
	case RenderLayer::Count:
		break;
	}
	return { 255, 255, 255 };
}

[[nodiscard]] size_t RenderLayerDiagnosticColorIndex(const uint8_t layerId)
{
	if (layerId < static_cast<uint8_t>(RenderLayer::Count))
		return layerId;
	return UnknownRenderLayerDiagnosticColorIndex;
}

[[nodiscard]] RgbColor RenderLayerDiagnosticColorByIndex(const size_t colorIndex)
{
	if (colorIndex < RenderLayerCount)
		return RenderLayerDiagnosticColor(static_cast<uint8_t>(colorIndex));
	return RenderLayerDiagnosticColor(UnknownRenderLayerId);
}

[[nodiscard]] RgbColor RenderWorldMaterialDiagnosticColor(const uint8_t materialId)
{
	switch (static_cast<RenderWorldMaterial>(materialId)) {
	case RenderWorldMaterial::Floor:
		return { 48, 180, 80 };
	case RenderWorldMaterial::LeftWall:
		return { 64, 112, 255 };
	case RenderWorldMaterial::RightWall:
		return { 64, 220, 255 };
	case RenderWorldMaterial::DoorArchBlocker:
		return { 255, 214, 64 };
	case RenderWorldMaterial::Actor:
		return { 255, 64, 220 };
	case RenderWorldMaterial::Object:
		return { 255, 144, 48 };
	case RenderWorldMaterial::Missile:
		return { 255, 64, 64 };
	case RenderWorldMaterial::Item:
		return { 64, 255, 180 };
	case RenderWorldMaterial::Corpse:
		return { 136, 136, 136 };
	case RenderWorldMaterial::SpecialSurface:
		return { 176, 64, 255 };
	case RenderWorldMaterial::Unknown:
	case RenderWorldMaterial::Count:
		break;
	}
	return { 64, 64, 64 };
}

[[nodiscard]] RgbColor RenderWorldProxyPrimitiveDiagnosticColor(const uint8_t primitiveId)
{
	switch (static_cast<RenderWorldProxyPrimitive>(primitiveId)) {
	case RenderWorldProxyPrimitive::FloorDiamond:
		return { 64, 255, 96 };
	case RenderWorldProxyPrimitive::LeftWallQuad:
		return { 255, 80, 80 };
	case RenderWorldProxyPrimitive::RightWallQuad:
		return { 80, 160, 255 };
	case RenderWorldProxyPrimitive::DoorArchBlocker:
		return { 255, 208, 64 };
	case RenderWorldProxyPrimitive::ObjectBlocker:
		return { 255, 128, 48 };
	case RenderWorldProxyPrimitive::ActorBillboard:
		return { 224, 64, 255 };
	case RenderWorldProxyPrimitive::Count:
		break;
	}
	return { 32, 32, 32 };
}

[[nodiscard]] RgbColor GrayscaleDiagnosticColor(const uint8_t value)
{
	return { value, value, value };
}

[[nodiscard]] size_t RenderWorldMaterialColorIndex(const uint8_t materialId)
{
	if (materialId < static_cast<uint8_t>(RenderWorldMaterial::Count))
		return materialId;
	return static_cast<size_t>(RenderWorldMaterial::Unknown);
}

[[nodiscard]] size_t RenderWorldProxyPrimitiveColorIndex(const uint8_t primitiveId)
{
	if (primitiveId < static_cast<uint8_t>(RenderWorldProxyPrimitive::Count))
		return primitiveId;
	return static_cast<size_t>(RenderWorldProxyPrimitive::Count);
}

[[nodiscard]] bool WorldMaskMapContains(const RenderWorldMaskMapView &map, const int x, const int y)
{
	return map.materialPixels != nullptr
	    && x >= 0
	    && y >= 0
	    && x < map.width
	    && y < map.height
	    && map.pitch >= map.width;
}

[[nodiscard]] bool WorldProxyMapContains(const RenderWorldProxyMapView &map, const int x, const int y)
{
	return map.typePixels != nullptr
	    && map.depthPixels != nullptr
	    && x >= 0
	    && y >= 0
	    && x < map.width
	    && y < map.height
	    && map.pitch >= map.width;
}

[[nodiscard]] bool WorldProxyHasPixel(const RenderWorldProxyMapView &map, const int x, const int y)
{
	if (!WorldProxyMapContains(map, x, y))
		return false;
	return map.typePixels[static_cast<size_t>(y) * map.pitch + x] != UnknownRenderWorldProxyPrimitiveId;
}

[[nodiscard]] bool WorldProxyEdgePixel(const RenderWorldProxyMapView &map, const int x, const int y)
{
	if (!WorldProxyHasPixel(map, x, y))
		return false;

	const size_t offset = static_cast<size_t>(y) * map.pitch + x;
	const uint8_t type = map.typePixels[offset];
	constexpr std::array<std::pair<int, int>, 4> Neighbors {
		std::pair { -1, 0 },
		std::pair { 1, 0 },
		std::pair { 0, -1 },
		std::pair { 0, 1 },
	};
	for (const auto [deltaX, deltaY] : Neighbors) {
		const Point position { x + deltaX, y + deltaY };
		if (!WorldProxyHasPixel(map, position.x, position.y))
			return true;
		const size_t neighborOffset = static_cast<size_t>(position.y) * map.pitch + position.x;
		if (map.typePixels[neighborOffset] != type)
			return true;
	}
	return false;
}

[[nodiscard]] uint32_t WorldMaskDiagnosticPixel(const CompositionFrame &frame, const SDL_Surface &outputSurface, const int x, const int y, const std::array<uint32_t, RenderWorldMaterialColorCount> &mappedMaterialColors)
{
	if (!WorldMaskMapContains(frame.worldMaskMap, x, y) || !IsWorldLayerPixel(frame.renderLayerMap, x, y))
		return 0;

	const size_t offset = static_cast<size_t>(y) * frame.worldMaskMap.pitch + x;
	switch (frame.renderWorldMaskDiagnosticMode) {
	case RenderWorldMaskDiagnosticMode::Material:
		return mappedMaterialColors[RenderWorldMaterialColorIndex(frame.worldMaskMap.materialPixels[offset])];
	case RenderWorldMaskDiagnosticMode::Receiver:
		return MapRgba(outputSurface, frame.worldMaskMap.receiverPixels != nullptr && frame.worldMaskMap.receiverPixels[offset] != 0 ? RgbColor { 64, 255, 96 } : RgbColor { 32, 32, 32 });
	case RenderWorldMaskDiagnosticMode::Occluder:
		return MapRgba(outputSurface, frame.worldMaskMap.occluderPixels != nullptr && frame.worldMaskMap.occluderPixels[offset] != 0 ? RgbColor { 64, 160, 255 } : RgbColor { 32, 32, 32 });
	case RenderWorldMaskDiagnosticMode::Emissive:
		return MapRgba(outputSurface, frame.worldMaskMap.emissivePixels != nullptr && frame.worldMaskMap.emissivePixels[offset] != 0 ? RgbColor { 255, 144, 48 } : RgbColor { 32, 32, 32 });
	case RenderWorldMaskDiagnosticMode::Off:
		break;
	}
	return 0;
}

[[nodiscard]] uint32_t WorldProxyDiagnosticPixel(
    const CompositionFrame &frame,
    const SDL_Surface &outputSurface,
    const int x,
    const int y,
    const std::array<uint32_t, RenderWorldProxyPrimitiveColorCount> &mappedProxyPrimitiveColors)
{
	if (!WorldProxyMapContains(frame.worldProxyMap, x, y) || !IsWorldLayerPixel(frame.renderLayerMap, x, y))
		return 0;

	const size_t offset = static_cast<size_t>(y) * frame.worldProxyMap.pitch + x;
	const bool hasProxy = frame.worldProxyMap.typePixels[offset] != UnknownRenderWorldProxyPrimitiveId;
	switch (frame.renderWorldProxyDiagnosticMode) {
	case RenderWorldProxyDiagnosticMode::Type:
		return mappedProxyPrimitiveColors[RenderWorldProxyPrimitiveColorIndex(frame.worldProxyMap.typePixels[offset])];
	case RenderWorldProxyDiagnosticMode::Coverage:
		return MapRgba(outputSurface, hasProxy ? RgbColor { 64, 255, 96 } : RgbColor { 255, 64, 64 });
	case RenderWorldProxyDiagnosticMode::Outline:
		if (!hasProxy)
			return MapRgba(outputSurface, { 255, 64, 64 });
		if (WorldProxyEdgePixel(frame.worldProxyMap, x, y))
			return MapRgba(outputSurface, { 255, 240, 64 });
		return 0;
	case RenderWorldProxyDiagnosticMode::Depth:
		return MapRgba(outputSurface, GrayscaleDiagnosticColor(frame.worldProxyMap.depthPixels[offset]));
	case RenderWorldProxyDiagnosticMode::Height:
		return MapRgba(outputSurface, { frame.worldProxyMap.heightPixels[offset], 64, static_cast<uint8_t>(255 - frame.worldProxyMap.heightPixels[offset]) });
	case RenderWorldProxyDiagnosticMode::Receiver:
		return MapRgba(outputSurface, frame.worldProxyMap.receiverPixels[offset] != 0 ? RgbColor { 64, 255, 96 } : RgbColor { 32, 32, 32 });
	case RenderWorldProxyDiagnosticMode::Occluder:
		return MapRgba(outputSurface, frame.worldProxyMap.occluderPixels[offset] != 0 ? RgbColor { 64, 160, 255 } : RgbColor { 32, 32, 32 });
	case RenderWorldProxyDiagnosticMode::Off:
		break;
	}
	return 0;
}

void PutPixelBytes(uint8_t *dst, const int bytesPerPixel, const uint32_t pixel)
{
	switch (bytesPerPixel) {
	case 1:
		*dst = static_cast<uint8_t>(pixel);
		break;
	case 2: {
		uint16_t value = static_cast<uint16_t>(pixel);
		std::memcpy(dst, &value, sizeof(value));
		break;
	}
	case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
		dst[0] = static_cast<uint8_t>((pixel >> 16) & 0xFF);
		dst[1] = static_cast<uint8_t>((pixel >> 8) & 0xFF);
		dst[2] = static_cast<uint8_t>(pixel & 0xFF);
#else
		dst[0] = static_cast<uint8_t>(pixel & 0xFF);
		dst[1] = static_cast<uint8_t>((pixel >> 8) & 0xFF);
		dst[2] = static_cast<uint8_t>((pixel >> 16) & 0xFF);
#endif
		break;
	case 4:
		std::memcpy(dst, &pixel, sizeof(pixel));
		break;
	default:
		break;
	}
}

[[nodiscard]] Rectangle ClipToBounds(Rectangle rect, const Size bounds)
{
	const int x0 = std::clamp(rect.position.x, 0, bounds.width);
	const int y0 = std::clamp(rect.position.y, 0, bounds.height);
	const int x1 = std::clamp(rect.position.x + rect.size.width, 0, bounds.width);
	const int y1 = std::clamp(rect.position.y + rect.size.height, 0, bounds.height);

	return { { x0, y0 }, { std::max(x1 - x0, 0), std::max(y1 - y0, 0) } };
}

[[nodiscard]] bool IsEmpty(const Rectangle &rect)
{
	return rect.size.width <= 0 || rect.size.height <= 0;
}

[[nodiscard]] bool IndexBufferIdentityChanged(const IndexBufferView &a, const IndexBufferView &b)
{
	return a.pixels != b.pixels
	    || a.width != b.width
	    || a.height != b.height
	    || a.pitch != b.pitch;
}

[[nodiscard]] bool Overlaps(const Rectangle &a, const Rectangle &b)
{
	return a.position.x < b.position.x + b.size.width
	    && b.position.x < a.position.x + a.size.width
	    && a.position.y < b.position.y + b.size.height
	    && b.position.y < a.position.y + a.size.height;
}

[[nodiscard]] Rectangle Union(const Rectangle &a, const Rectangle &b)
{
	const int x0 = std::min(a.position.x, b.position.x);
	const int y0 = std::min(a.position.y, b.position.y);
	const int x1 = std::max(a.position.x + a.size.width, b.position.x + b.size.width);
	const int y1 = std::max(a.position.y + a.size.height, b.position.y + b.size.height);
	return { { x0, y0 }, { x1 - x0, y1 - y0 } };
}

void AccumulateCompositionSurfaceDirtyRect(CompositionSurfaceMetadata &metadata, const CompositionSurfaceRole role, const Rectangle rect)
{
	if (IsEmpty(rect))
		return;
	const size_t roleIndex = CompositionSurfaceRoleIndex(role);
	if (roleIndex >= CompositionSurfaceRoleCount)
		return;

	CompositionSurfaceRoleCoverage &coverage = metadata.roles[roleIndex];
	coverage.dirtyBounds = coverage.dirtyRectCount == 0 ? rect : Union(coverage.dirtyBounds, rect);
	coverage.dirtyRectCount++;
	coverage.dirtyPixelArea += Area(rect);
}

void AccumulateCompositionSurfaceFullFrame(CompositionSurfaceMetadata &metadata, const CompositionSurfaceRole role)
{
	const size_t roleIndex = CompositionSurfaceRoleIndex(role);
	if (roleIndex >= CompositionSurfaceRoleCount)
		return;
	metadata.roles[roleIndex].fullFrameDirty = true;
}

[[nodiscard]] CompositionSurfaceMetadata WithFullFrameCompositionSurfaceBounds(CompositionSurfaceMetadata metadata, const Size logicalSize)
{
	if (logicalSize.width <= 0 || logicalSize.height <= 0)
		return metadata;

	const Rectangle fullFrame { { 0, 0 }, logicalSize };
	const uint64_t fullFrameArea = Area(logicalSize);
	for (CompositionSurfaceRoleCoverage &coverage : metadata.roles) {
		if (!coverage.fullFrameDirty)
			continue;
		coverage.dirtyBounds = fullFrame;
		coverage.dirtyPixelArea = std::max(coverage.dirtyPixelArea, fullFrameArea);
		if (coverage.dirtyRectCount == 0)
			coverage.dirtyRectCount = 1;
	}
	return metadata;
}

[[nodiscard]] CompositionSurfaceRole CompositionSurfaceRoleForRenderLayer(const RenderLayer layer)
{
	switch (layer) {
	case RenderLayer::World:
		return CompositionSurfaceRole::World;
	case RenderLayer::WorldOverlay:
		return CompositionSurfaceRole::WorldOverlay;
	case RenderLayer::Interface:
		return CompositionSurfaceRole::Interface;
	case RenderLayer::Cursor:
		return CompositionSurfaceRole::Cursor;
	case RenderLayer::Debug:
	case RenderLayer::Count:
		return CompositionSurfaceRole::DiagnosticOverlay;
	}
	return CompositionSurfaceRole::DiagnosticOverlay;
}

[[nodiscard]] CompositionAttachmentRole CompositionAttachmentRoleForSurfaceRole(const CompositionSurfaceRole role)
{
	switch (role) {
	case CompositionSurfaceRole::World:
		return CompositionAttachmentRole::WorldIndex;
	case CompositionSurfaceRole::WorldOverlay:
		return CompositionAttachmentRole::WorldOverlayIndex;
	case CompositionSurfaceRole::Interface:
		return CompositionAttachmentRole::InterfaceIndex;
	case CompositionSurfaceRole::Cursor:
		return CompositionAttachmentRole::CursorIndex;
	case CompositionSurfaceRole::DiagnosticOverlay:
	case CompositionSurfaceRole::Count:
		return CompositionAttachmentRole::Diagnostic;
	}
	return CompositionAttachmentRole::Diagnostic;
}

[[nodiscard]] DirtyRectList DirtyRectsForCompositionSurfaceRole(const CompositionSurfaceMetadata &metadata, const CompositionSurfaceRole role)
{
	DirtyRectList dirtyRects;
	const size_t roleIndex = CompositionSurfaceRoleIndex(role);
	if (roleIndex >= CompositionSurfaceRoleCount)
		return dirtyRects;

	const CompositionSurfaceRoleCoverage &coverage = metadata.roles[roleIndex];
	if (coverage.fullFrameDirty) {
		dirtyRects.fullFrame = true;
		return dirtyRects;
	}
	if (coverage.dirtyRectCount > 0 && !IsEmpty(coverage.dirtyBounds))
		dirtyRects.rects.push_back(coverage.dirtyBounds);
	return dirtyRects;
}

[[nodiscard]] CompositionAttachment MakeSharedIndexSurfaceRoleAttachment(const CompositionSurfaceMetadata &metadata, const CompositionSurfaceRole role, const IndexBufferView indexBuffer, const Size logicalSize)
{
	return {
		CompositionAttachmentRoleForSurfaceRole(role),
		CompositionAttachmentFormat::Index8,
		logicalSize,
		indexBuffer.pitch,
		indexBuffer.version,
		DirtyRectsForCompositionSurfaceRole(metadata, role),
		indexBuffer.pixels,
	};
}

void UpsertCompositionSurfaceRoleAttachments(std::vector<CompositionAttachment> &attachments, const CompositionSurfaceMetadata &metadata, const IndexBufferView indexBuffer, const Size logicalSize)
{
	UpsertCompositionAttachment(attachments, MakeSharedIndexSurfaceRoleAttachment(metadata, CompositionSurfaceRole::World, indexBuffer, logicalSize));
	UpsertCompositionAttachment(attachments, MakeSharedIndexSurfaceRoleAttachment(metadata, CompositionSurfaceRole::WorldOverlay, indexBuffer, logicalSize));
	UpsertCompositionAttachment(attachments, MakeSharedIndexSurfaceRoleAttachment(metadata, CompositionSurfaceRole::Interface, indexBuffer, logicalSize));
	UpsertCompositionAttachment(attachments, MakeSharedIndexSurfaceRoleAttachment(metadata, CompositionSurfaceRole::Cursor, indexBuffer, logicalSize));
	UpsertCompositionAttachment(attachments, MakeSharedIndexSurfaceRoleAttachment(metadata, CompositionSurfaceRole::DiagnosticOverlay, indexBuffer, logicalSize));
}

[[nodiscard]] bool WorldMaskMapIsValid(const RenderWorldMaskMapView &map, const Size logicalSize)
{
	return map.materialPixels != nullptr
	    && map.receiverPixels != nullptr
	    && map.occluderPixels != nullptr
	    && map.width >= logicalSize.width
	    && map.height >= logicalSize.height
	    && map.pitch >= map.width
	    && logicalSize.width > 0
	    && logicalSize.height > 0;
}

[[nodiscard]] CompositionAttachment MakeWorldMaskAttachment(const CompositionAttachmentRole role, const uint8_t *pixels, const RenderWorldMaskMapView &map, const Size logicalSize, const DirtyRectList &dirtyRects)
{
	return {
		role,
		role == CompositionAttachmentRole::WorldMaterial ? CompositionAttachmentFormat::Index8 : CompositionAttachmentFormat::Alpha8,
		logicalSize,
		map.pitch,
		map.version,
		dirtyRects,
		pixels,
	};
}

void UpsertWorldMaskAttachments(std::vector<CompositionAttachment> &attachments, const RenderWorldMaskMapView &map, const Size logicalSize, const DirtyRectList &dirtyRects)
{
	if (!WorldMaskMapIsValid(map, logicalSize))
		return;

	UpsertCompositionAttachment(attachments, MakeWorldMaskAttachment(CompositionAttachmentRole::WorldMaterial, map.materialPixels, map, logicalSize, dirtyRects));
	UpsertCompositionAttachment(attachments, MakeWorldMaskAttachment(CompositionAttachmentRole::WorldReceiver, map.receiverPixels, map, logicalSize, dirtyRects));
	UpsertCompositionAttachment(attachments, MakeWorldMaskAttachment(CompositionAttachmentRole::WorldOccluder, map.occluderPixels, map, logicalSize, dirtyRects));
}

[[nodiscard]] bool WorldProxyMapIsValid(const RenderWorldProxyMapView &map, const Size logicalSize)
{
	return map.typePixels != nullptr
	    && map.depthPixels != nullptr
	    && map.heightPixels != nullptr
	    && map.receiverPixels != nullptr
	    && map.occluderPixels != nullptr
	    && map.width >= logicalSize.width
	    && map.height >= logicalSize.height
	    && map.pitch >= map.width
	    && logicalSize.width > 0
	    && logicalSize.height > 0;
}

[[nodiscard]] CompositionAttachment MakeWorldProxyAttachment(const CompositionAttachmentRole role, const uint8_t *pixels, const RenderWorldProxyMapView &map, const Size logicalSize, const DirtyRectList &dirtyRects)
{
	return {
		role,
		CompositionAttachmentFormat::Alpha8,
		logicalSize,
		map.pitch,
		map.version,
		dirtyRects,
		pixels,
	};
}

void UpsertWorldProxyAttachments(std::vector<CompositionAttachment> &attachments, const RenderWorldProxyMapView &map, const Size logicalSize, const DirtyRectList &dirtyRects)
{
	if (!WorldProxyMapIsValid(map, logicalSize))
		return;

	UpsertCompositionAttachment(attachments, MakeWorldProxyAttachment(CompositionAttachmentRole::WorldDepth, map.depthPixels, map, logicalSize, dirtyRects));
	UpsertCompositionAttachment(attachments, MakeWorldProxyAttachment(CompositionAttachmentRole::WorldHeight, map.heightPixels, map, logicalSize, dirtyRects));
	UpsertCompositionAttachment(attachments, MakeWorldProxyAttachment(CompositionAttachmentRole::WorldReceiver, map.receiverPixels, map, logicalSize, dirtyRects));
	UpsertCompositionAttachment(attachments, MakeWorldProxyAttachment(CompositionAttachmentRole::WorldOccluder, map.occluderPixels, map, logicalSize, dirtyRects));
}

void RecordCompositionSurfaceStats(RenderPerfCompositionStats &stats, const CompositionSurfaceMetadata &metadata)
{
	const auto coverage = [&](const CompositionSurfaceRole role) -> const CompositionSurfaceRoleCoverage & {
		return metadata.roles[CompositionSurfaceRoleIndex(role)];
	};

	stats.worldRoleDirtyRectCount = coverage(CompositionSurfaceRole::World).dirtyRectCount;
	stats.worldOverlayRoleDirtyRectCount = coverage(CompositionSurfaceRole::WorldOverlay).dirtyRectCount;
	stats.interfaceRoleDirtyRectCount = coverage(CompositionSurfaceRole::Interface).dirtyRectCount;
	stats.cursorRoleDirtyRectCount = coverage(CompositionSurfaceRole::Cursor).dirtyRectCount;
	stats.diagnosticOverlayRoleDirtyRectCount = coverage(CompositionSurfaceRole::DiagnosticOverlay).dirtyRectCount;
	stats.worldRoleDirtyPixelArea = coverage(CompositionSurfaceRole::World).dirtyPixelArea;
	stats.worldOverlayRoleDirtyPixelArea = coverage(CompositionSurfaceRole::WorldOverlay).dirtyPixelArea;
	stats.interfaceRoleDirtyPixelArea = coverage(CompositionSurfaceRole::Interface).dirtyPixelArea;
	stats.cursorRoleDirtyPixelArea = coverage(CompositionSurfaceRole::Cursor).dirtyPixelArea;
	stats.diagnosticOverlayRoleDirtyPixelArea = coverage(CompositionSurfaceRole::DiagnosticOverlay).dirtyPixelArea;
}

void MergeOverlapsAt(std::vector<Rectangle> &rects, size_t index)
{
	bool merged = true;
	while (merged) {
		merged = false;
		for (size_t i = 0; i < rects.size(); i++) {
			if (i == index || !Overlaps(rects[index], rects[i]))
				continue;

			rects[index] = Union(rects[index], rects[i]);
			rects.erase(rects.begin() + static_cast<ptrdiff_t>(i));
			if (i < index)
				index--;
			merged = true;
			break;
		}
	}
}

[[nodiscard]] int CompositionThreadCount(const Rectangle &rect)
{
#ifdef BUILD_TESTING
	if (ThreadCountOverrideForTesting > 0) {
		int threadCount = std::min(ThreadCountOverrideForTesting, std::max(1, rect.size.height));
#if DEVILUTIONX_PARALLEL_COMPOSITION
		threadCount = std::min(threadCount, static_cast<int>(MaxParallelCompositionThreads));
#endif
		return threadCount;
	}
#endif
#if DEVILUTIONX_PARALLEL_COMPOSITION
	const unsigned hardwareThreadCount = std::thread::hardware_concurrency();
	if (hardwareThreadCount <= 1)
		return 1;

	const size_t pixelCount = static_cast<size_t>(rect.size.width) * static_cast<size_t>(rect.size.height);
	if (pixelCount < MinParallelCompositionPixels)
		return 1;

	const unsigned rowLimitedThreadCount = std::max(1, rect.size.height / MinParallelCompositionRowsPerThread);
	const unsigned pixelLimitedThreadCount = static_cast<unsigned>((pixelCount + MinParallelCompositionPixels - 1) / MinParallelCompositionPixels);
	return static_cast<int>(std::min({ hardwareThreadCount, rowLimitedThreadCount, pixelLimitedThreadCount, MaxParallelCompositionThreads }));
#else
	(void)rect;
	return 1;
#endif
}

void AddNormalizedDirtyRect(DirtyRectList &dirtyRects, Rectangle rect)
{
	for (size_t i = 0; i < dirtyRects.rects.size(); i++) {
		if (!Overlaps(dirtyRects.rects[i], rect))
			continue;

		dirtyRects.rects[i] = Union(dirtyRects.rects[i], rect);
		MergeOverlapsAt(dirtyRects.rects, i);
		return;
	}

	dirtyRects.rects.push_back(rect);
}

[[nodiscard]] NormalizedDirtyRects NormalizeDirtyRects(const DirtyRectList &dirtyRects, const Size bounds)
{
	NormalizedDirtyRects normalized;
	normalized.submittedRectCount = static_cast<int>(dirtyRects.rects.size());
	for (const Rectangle &rect : dirtyRects.rects) {
		normalized.submittedArea += Area(rect);
	}

	if (dirtyRects.fullFrame) {
		normalized.dirtyRects.fullFrame = true;
		normalized.normalizedArea = Area(bounds);
		return normalized;
	}

	for (Rectangle rect : dirtyRects.rects) {
		if (IsEmpty(rect))
			continue;

		rect = ClipToBounds(rect, bounds);
		if (IsEmpty(rect))
			continue;

		AddNormalizedDirtyRect(normalized.dirtyRects, rect);
		if (normalized.dirtyRects.rects.size() > MaxDirtyRectsBeforeFullFrame) {
			normalized.dirtyRects.rects.clear();
			normalized.dirtyRects.fullFrame = true;
			normalized.tooManyDirtyRects = true;
			normalized.normalizedArea = Area(bounds);
			return normalized;
		}
	}

	normalized.normalizedRectCount = static_cast<int>(normalized.dirtyRects.rects.size());
	for (const Rectangle &rect : normalized.dirtyRects.rects) {
		normalized.normalizedArea += Area(rect);
	}
	return normalized;
}

class CpuPaletteCompositorBackend final : public IFrameCompositorBackend {
public:
	std::string_view Name() const override
	{
		return "cpu-palette";
	}

	bool IsAvailable() const override
	{
		return true;
	}

	FrameCompositorBackendResult Compose(const CompositionFrame &frame, SDL_Surface &outputSurface, const std::vector<Rectangle> &rects, RenderPerfCompositionStats &stats) override
	{
		bool composed = false;
		for (const Rectangle &rect : rects) {
			if (ComposeRect(frame, outputSurface, rect, stats))
				composed = true;
		}
		return composed ? FrameCompositorBackendResult::UpdatedOutputSurface : FrameCompositorBackendResult::NoFrameProduced;
	}

private:
	struct MappedPaletteCache {
		bool valid = false;
		SDL_Surface *outputSurface = nullptr;
		uintptr_t outputFormatIdentity = 0;
		uint64_t paletteVersion = 0;
		bool diagnosticTransform = false;
		std::array<uint32_t, 256> mappedPalette {};
	};

	[[nodiscard]] const std::array<uint32_t, 256> &GetMappedPalette(SDL_Surface &outputSurface, const PaletteSnapshot &palette, bool diagnosticTransform)
	{
		const uintptr_t outputFormatIdentity = OutputSurfaceFormatIdentity(outputSurface);
		if (mappedPaletteCache_.valid
		    && mappedPaletteCache_.outputSurface == &outputSurface
		    && mappedPaletteCache_.outputFormatIdentity == outputFormatIdentity
		    && mappedPaletteCache_.paletteVersion == palette.version
		    && mappedPaletteCache_.diagnosticTransform == diagnosticTransform) {
			return mappedPaletteCache_.mappedPalette;
		}

		for (size_t i = 0; i < mappedPaletteCache_.mappedPalette.size(); i++) {
			const RgbColor color = diagnosticTransform ? ApplyDiagnosticTransform(palette.colors[i]) : palette.colors[i];
			mappedPaletteCache_.mappedPalette[i] = MapRgba(outputSurface, color);
		}
		mappedPaletteCache_.valid = true;
		mappedPaletteCache_.outputSurface = &outputSurface;
		mappedPaletteCache_.outputFormatIdentity = outputFormatIdentity;
		mappedPaletteCache_.paletteVersion = palette.version;
		mappedPaletteCache_.diagnosticTransform = diagnosticTransform;
		return mappedPaletteCache_.mappedPalette;
	}

	[[nodiscard]] bool ComposeRect(const CompositionFrame &frame, SDL_Surface &outputSurface, Rectangle rect, RenderPerfCompositionStats &stats)
	{
		if (frame.indexBuffer.pixels == nullptr)
			return false;

		const Size bounds {
			std::min({ frame.logicalSize.width, frame.indexBuffer.width, outputSurface.w }),
			std::min({ frame.logicalSize.height, frame.indexBuffer.height, outputSurface.h }),
		};
		rect = ClipToBounds(rect, bounds);
		if (rect.size.width == 0 || rect.size.height == 0)
			return false;

		SDL_Surface *outputSurfacePtr = &outputSurface;
		const bool mustLock = SDL_MUSTLOCK(outputSurfacePtr);
		if (mustLock) {
#ifdef USE_SDL3
			if (!SDL_LockSurface(outputSurfacePtr)) ErrSdl();
#else
			if (SDL_LockSurface(outputSurfacePtr) < 0) ErrSdl();
#endif
		}

		const bool renderLayerDiagnosticsEnabled = frame.renderLayerDiagnosticMode != RenderLayerDiagnosticMode::Off && frame.renderLayerMap.pixels != nullptr;
		const bool renderLayerTintEnabled = renderLayerDiagnosticsEnabled && UsesRenderLayerTint(frame.renderLayerDiagnosticMode);
		const bool renderLayerOutlineEnabled = renderLayerDiagnosticsEnabled && UsesRenderLayerOutline(frame.renderLayerDiagnosticMode);
		const bool worldMaskDiagnosticsEnabled = WorldMaskDiagnosticEnabled(frame);
		const bool worldProxyDiagnosticsEnabled = WorldProxyDiagnosticEnabled(frame);
		const std::array<uint32_t, 256> &mappedPalette = GetMappedPalette(outputSurface, frame.palette, frame.diagnosticTransform);
		std::array<std::array<uint32_t, 256>, RenderLayerDiagnosticColorCount> mappedTintPalettes {};
		std::array<uint32_t, RenderLayerDiagnosticColorCount> mappedOutlineColors {};
		std::array<uint32_t, RenderWorldMaterialColorCount> mappedWorldMaterialColors {};
		std::array<uint32_t, RenderWorldProxyPrimitiveColorCount> mappedWorldProxyPrimitiveColors {};
		if (renderLayerTintEnabled) {
			for (size_t i = 0; i < mappedPalette.size(); i++) {
				const RgbColor color = frame.diagnosticTransform ? ApplyDiagnosticTransform(frame.palette.colors[i]) : frame.palette.colors[i];
				for (size_t colorIndex = 0; colorIndex < mappedTintPalettes.size(); colorIndex++) {
					RgbColor tintedColor = color;
					const RgbColor layerColor = RenderLayerDiagnosticColorByIndex(colorIndex);
					tintedColor.r = static_cast<uint8_t>((static_cast<uint16_t>(tintedColor.r) + layerColor.r) / 2);
					tintedColor.g = static_cast<uint8_t>((static_cast<uint16_t>(tintedColor.g) + layerColor.g) / 2);
					tintedColor.b = static_cast<uint8_t>((static_cast<uint16_t>(tintedColor.b) + layerColor.b) / 2);
					mappedTintPalettes[colorIndex][i] = MapRgba(outputSurface, tintedColor);
				}
			}
		}
		if (renderLayerOutlineEnabled) {
			for (size_t colorIndex = 0; colorIndex < mappedOutlineColors.size(); colorIndex++)
				mappedOutlineColors[colorIndex] = MapRgba(outputSurface, RenderLayerDiagnosticColorByIndex(colorIndex));
		}
		if (worldMaskDiagnosticsEnabled) {
			for (size_t materialIndex = 0; materialIndex < mappedWorldMaterialColors.size(); materialIndex++)
				mappedWorldMaterialColors[materialIndex] = MapRgba(outputSurface, RenderWorldMaterialDiagnosticColor(static_cast<uint8_t>(materialIndex)));
		}
		if (worldProxyDiagnosticsEnabled) {
			for (size_t primitiveIndex = 0; primitiveIndex < mappedWorldProxyPrimitiveColors.size(); primitiveIndex++)
				mappedWorldProxyPrimitiveColors[primitiveIndex] = MapRgba(outputSurface, RenderWorldProxyPrimitiveDiagnosticColor(static_cast<uint8_t>(primitiveIndex)));
		}

		const int bytesPerPixel = BytesPerPixel(outputSurface);
		const bool useFast32NoDiagnostics = bytesPerPixel == 4 && !frame.diagnosticTransform && !renderLayerDiagnosticsEnabled && !worldMaskDiagnosticsEnabled && !worldProxyDiagnosticsEnabled;
		const auto composeRows32NoDiagnostics = [&](const int yBegin, const int yEnd) {
			for (int y = yBegin; y < yEnd; y++) {
				const uint8_t *src = frame.indexBuffer.pixels + y * frame.indexBuffer.pitch + rect.position.x;
				uint32_t *dst = reinterpret_cast<uint32_t *>(
				    static_cast<uint8_t *>(outputSurface.pixels) + static_cast<ptrdiff_t>(y) * outputSurface.pitch + rect.position.x * sizeof(uint32_t));
				for (int x = 0; x < rect.size.width; x++) {
					dst[x] = mappedPalette[src[x]];
				}
			}
		};
		const auto composeRowsGeneric = [&](const int yBegin, const int yEnd) {
			for (int y = yBegin; y < yEnd; y++) {
				const uint8_t *src = frame.indexBuffer.pixels + y * frame.indexBuffer.pitch + rect.position.x;
				uint8_t *dstRow = static_cast<uint8_t *>(outputSurface.pixels) + static_cast<ptrdiff_t>(y) * outputSurface.pitch + rect.position.x * bytesPerPixel;
				uint32_t *dst32 = bytesPerPixel == 4 ? reinterpret_cast<uint32_t *>(dstRow) : nullptr;
				const bool layerRowInBounds = frame.renderLayerMap.pixels != nullptr && y >= 0 && y < frame.renderLayerMap.height;
				const uint8_t *layerRow = layerRowInBounds ? frame.renderLayerMap.pixels + static_cast<size_t>(y) * frame.renderLayerMap.pitch : nullptr;
				const uint8_t *layerRowAbove = frame.renderLayerMap.pixels != nullptr && y > 0 && y - 1 < frame.renderLayerMap.height ? frame.renderLayerMap.pixels + static_cast<size_t>(y - 1) * frame.renderLayerMap.pitch : nullptr;
				const uint8_t *layerRowBelow = frame.renderLayerMap.pixels != nullptr && y + 1 < frame.renderLayerMap.height ? frame.renderLayerMap.pixels + static_cast<size_t>(y + 1) * frame.renderLayerMap.pitch : nullptr;
				for (int x = 0; x < rect.size.width; x++) {
					const int outputX = rect.position.x + x;
					uint32_t pixel = mappedPalette[src[x]];
					if (renderLayerDiagnosticsEnabled) {
						uint8_t layerId = UnknownRenderLayerId;
						if (layerRow != nullptr && outputX >= 0 && outputX < frame.renderLayerMap.width)
							layerId = layerRow[outputX];
						const size_t colorIndex = RenderLayerDiagnosticColorIndex(layerId);
						if (renderLayerTintEnabled)
							pixel = mappedTintPalettes[colorIndex][src[x]];
						if (renderLayerOutlineEnabled) {
							bool isBoundary = layerId == UnknownRenderLayerId;
							if (!isBoundary) {
								if (outputX > 0 && layerRow[outputX - 1] != layerId)
									isBoundary = true;
								else if (outputX + 1 < frame.renderLayerMap.width && layerRow[outputX + 1] != layerId)
									isBoundary = true;
								else if (layerRowAbove != nullptr && layerRowAbove[outputX] != layerId)
									isBoundary = true;
								else if (layerRowBelow != nullptr && layerRowBelow[outputX] != layerId)
									isBoundary = true;
							}
							if (isBoundary)
								pixel = mappedOutlineColors[colorIndex];
						}
					}
					if (worldMaskDiagnosticsEnabled) {
						const uint32_t diagnosticPixel = WorldMaskDiagnosticPixel(frame, outputSurface, outputX, y, mappedWorldMaterialColors);
						if (diagnosticPixel != 0)
							pixel = diagnosticPixel;
					}
					if (worldProxyDiagnosticsEnabled) {
						const uint32_t diagnosticPixel = WorldProxyDiagnosticPixel(frame, outputSurface, outputX, y, mappedWorldProxyPrimitiveColors);
						if (diagnosticPixel != 0)
							pixel = diagnosticPixel;
					}
					if (dst32 != nullptr) {
						dst32[x] = pixel;
					} else {
						PutPixelBytes(dstRow + x * bytesPerPixel, bytesPerPixel, pixel);
					}
				}
			}
		};
		const auto composeRows = [&](const int yBegin, const int yEnd) {
			if (useFast32NoDiagnostics) {
				composeRows32NoDiagnostics(yBegin, yEnd);
			} else {
				composeRowsGeneric(yBegin, yEnd);
			}
		};

		const int threadCount = CompositionThreadCount(rect);
		stats.selectedThreadCount = std::max(stats.selectedThreadCount, threadCount);
#if DEVILUTIONX_PARALLEL_COMPOSITION
		if (threadCount > 1)
			stats.parallelCompositionUsed = true;
#endif
#if DEVILUTIONX_PARALLEL_COMPOSITION
		if (threadCount == 1) {
			composeRows(rect.position.y, rect.position.y + rect.size.height);
		} else {
			CompositionWorkers().Run(threadCount, rect.position.y, rect.position.y + rect.size.height, composeRows);
		}
#else
		(void)threadCount;
		composeRows(rect.position.y, rect.position.y + rect.size.height);
#endif

		if (mustLock)
			SDL_UnlockSurface(outputSurfacePtr);
		return true;
	}

	MappedPaletteCache mappedPaletteCache_ {};
};

} // namespace

IndexBufferView MakeIndexBufferView(const SDL_Surface &surface)
{
	if (SDLC_SURFACE_BITSPERPIXEL((&surface)) != 8) {
		return {};
	}

	return {
		static_cast<const uint8_t *>(surface.pixels),
		surface.w,
		surface.h,
		surface.pitch,
	};
}

PaletteSnapshot MakePaletteSnapshot(const std::array<SDL_Color, 256> &palette, const uint64_t version)
{
	PaletteSnapshot snapshot;
	snapshot.version = version;
	for (size_t i = 0; i < snapshot.colors.size(); i++) {
		snapshot.colors[i] = { palette[i].r, palette[i].g, palette[i].b,
#ifndef USE_SDL1
			palette[i].a
#else
			255
#endif
		};
	}
	return snapshot;
}

CompositionAttachment MakeIndexedAlbedoAttachment(const IndexBufferView indexBuffer, const Size logicalSize, const DirtyRectList &dirtyRects)
{
	return {
		CompositionAttachmentRole::IndexedAlbedo,
		CompositionAttachmentFormat::Index8,
		logicalSize,
		indexBuffer.pitch,
		indexBuffer.version,
		dirtyRects,
		indexBuffer.pixels,
	};
}

CompositionAttachment MakePaletteAttachment(const PaletteSnapshot &palette)
{
	return {
		CompositionAttachmentRole::Palette,
		CompositionAttachmentFormat::PaletteRgba8,
		{ 256, 1 },
		256 * 4,
		palette.version,
		{},
		reinterpret_cast<const uint8_t *>(palette.colors.data()),
	};
}

const CompositionAttachment *FindCompositionAttachment(const std::span<const CompositionAttachment> attachments, const CompositionAttachmentRole role)
{
	for (const CompositionAttachment &attachment : attachments) {
		if (attachment.role == role)
			return &attachment;
	}
	return nullptr;
}

CompositionAttachmentUploadPlan PlanCompositionAttachmentUpload(const CompositionAttachment &attachment, const bool alreadyUploaded, const uint64_t uploadedVersion)
{
	CompositionAttachmentUploadPlan plan;
	const int bytesPerPixel = CompositionAttachmentBytesPerPixel(attachment.format);
	if (attachment.cpuPixels == nullptr || attachment.logicalSize.width <= 0 || attachment.logicalSize.height <= 0 || attachment.pitch <= 0 || bytesPerPixel <= 0)
		return plan;

	const Rectangle fullRect { { 0, 0 }, attachment.logicalSize };
	const auto addRect = [&](Rectangle rect) {
		rect = ClipToBounds(rect, attachment.logicalSize);
		if (IsEmpty(rect))
			return;
		plan.rects.push_back(rect);
		plan.byteCount += Area(rect) * static_cast<uint64_t>(bytesPerPixel);
	};

	if (!alreadyUploaded || attachment.dirtyRects.fullFrame) {
		plan.action = CompositionAttachmentUploadAction::Full;
		addRect(fullRect);
		return plan;
	}

	if (!attachment.dirtyRects.rects.empty()) {
		for (const Rectangle &rect : attachment.dirtyRects.rects) {
			addRect(rect);
		}
		if (!plan.rects.empty()) {
			plan.action = plan.rects.size() == 1 && plan.rects[0].position.x == 0 && plan.rects[0].position.y == 0
			        && plan.rects[0].size == attachment.logicalSize
			    ? CompositionAttachmentUploadAction::Full
			    : CompositionAttachmentUploadAction::Partial;
		}
		return plan;
	}

	if (attachment.version != uploadedVersion) {
		plan.action = CompositionAttachmentUploadAction::Full;
		addRect(fullRect);
	}
	return plan;
}

std::unique_ptr<IFrameCompositorBackend> CreateCpuFrameCompositorBackend()
{
	return std::make_unique<CpuPaletteCompositorBackend>();
}

namespace {

RenderFrameCompositorBackend CurrentFrameCompositorBackend = RenderFrameCompositorBackend::CpuPalette;
bool CurrentFrameCompositorBackendInitialized = false;

[[nodiscard]] std::unique_ptr<IFrameCompositorBackend> CreateFrameCompositorBackend(RenderFrameCompositorBackend backend)
{
	std::unique_ptr<IFrameCompositorBackend> acceleratedBackend = CreateAcceleratedFrameCompositorBackend(backend);
	if (acceleratedBackend != nullptr && acceleratedBackend->IsAvailable())
		return acceleratedBackend;
	return CreateCpuFrameCompositorBackend();
}

void EnsureFrameCompositorBackend()
{
	const RenderFrameCompositorBackend requestedBackend = *GetOptions().Experimental.renderFrameCompositorBackend;
	if (CurrentFrameCompositorBackendInitialized && CurrentFrameCompositorBackend == requestedBackend)
		return;

	FrameCompositor.SetBackend(CreateFrameCompositorBackend(requestedBackend));
	CurrentFrameCompositorBackend = requestedBackend;
	CurrentFrameCompositorBackendInitialized = true;
}

} // namespace

#ifdef BUILD_TESTING
void SetFrameCompositorThreadCountOverrideForTesting(const int threadCount)
{
	ThreadCountOverrideForTesting = std::max(0, threadCount);
}
#endif

CpuPaletteCompositor::CpuPaletteCompositor()
    : CpuPaletteCompositor(CreateCpuFrameCompositorBackend())
{
}

CpuPaletteCompositor::CpuPaletteCompositor(std::unique_ptr<IFrameCompositorBackend> backend)
    : backend_(std::move(backend))
{
}

void CpuPaletteCompositor::BeginFrame(const Size logicalSize)
{
	if (logicalSize_ != logicalSize) {
		if (hasComposedFrame_)
			logicalSizeChangedSinceComposition_ = true;
		hasComposedFrame_ = false;
	}
	logicalSize_ = logicalSize;
}

void CpuPaletteCompositor::SubmitIndexBuffer(const IndexBufferView indexBuffer)
{
	if (IndexBufferIdentityChanged(indexBuffer_, indexBuffer)) {
		if (hasComposedFrame_)
			indexBufferChangedSinceComposition_ = true;
		hasComposedFrame_ = false;
	}
	indexBuffer_ = indexBuffer;
}

void CpuPaletteCompositor::SubmitPalette(const PaletteSnapshot &palette)
{
	palette_ = palette;
}

void CpuPaletteCompositor::SubmitDirtyRects(const DirtyRectList &dirtyRects)
{
	dirtyRects_ = dirtyRects;
}

void CpuPaletteCompositor::SetOutputSurface(SDL_Surface *outputSurface)
{
	if (outputSurface_ != outputSurface) {
		if (hasComposedFrame_)
			outputSurfaceChangedSinceComposition_ = true;
		hasComposedFrame_ = false;
	}
	outputSurface_ = outputSurface;
}

void CpuPaletteCompositor::AddDirtyRect(Rectangle rect)
{
	AddDirtyRect(rect, CompositionSurfaceRole::World);
}

void CpuPaletteCompositor::AddDirtyRect(Rectangle rect, const CompositionSurfaceRole role)
{
	if (rect.size.width <= 0 || rect.size.height <= 0)
		return;
	dirtyRects_.rects.push_back(rect);
	AccumulateCompositionSurfaceDirtyRect(compositionSurfaceMetadata_, role, rect);
}

void CpuPaletteCompositor::SetFullFrameDirty()
{
	SetFullFrameDirty(CompositionSurfaceRole::World);
}

void CpuPaletteCompositor::SetFullFrameDirty(const CompositionSurfaceRole role)
{
	dirtyRects_.fullFrame = true;
	AccumulateCompositionSurfaceFullFrame(compositionSurfaceMetadata_, role);
}

void CpuPaletteCompositor::ResetDirtyRects()
{
	dirtyRects_ = {};
	compositionSurfaceMetadata_ = {};
}

void CpuPaletteCompositor::SetDiagnosticTransformEnabled(const bool enabled)
{
	diagnosticTransformEnabled_ = enabled;
}

void CpuPaletteCompositor::SetBackend(std::unique_ptr<IFrameCompositorBackend> backend)
{
	backend_ = std::move(backend);
	hasComposedFrame_ = false;
	lastBackendResult_ = FrameCompositorBackendResult::NoFrameProduced;
	directPresentationPending_ = false;
	lastComposedFrameUsedDirectPresentation_ = false;
	outputSurfaceChangedSinceComposition_ = true;
}

const DirtyRectList &CpuPaletteCompositor::GetDirtyRects() const
{
	return dirtyRects_;
}

const CompositionSurfaceMetadata &CpuPaletteCompositor::GetCompositionSurfaceMetadata() const
{
	return compositionSurfaceMetadata_;
}

const RenderPerfCompositionStats &CpuPaletteCompositor::GetLastCompositionStats() const
{
	return lastCompositionStats_;
}

FrameCompositorBackendResult CpuPaletteCompositor::GetLastBackendResult() const
{
	return lastBackendResult_;
}

void CpuPaletteCompositor::Compose(const CompositionFrame &frame)
{
	const bool logicalSizeChanged = logicalSizeChangedSinceComposition_ || (hasComposedFrame_ && logicalSize_ != frame.logicalSize);
	const bool indexBufferChanged = indexBufferChangedSinceComposition_ || (hasComposedFrame_ && IndexBufferIdentityChanged(indexBuffer_, frame.indexBuffer));
	const bool outputSurfaceChanged = outputSurfaceChangedSinceComposition_;
	if (logicalSizeChanged)
		hasComposedFrame_ = false;
	if (indexBufferChanged)
		hasComposedFrame_ = false;
	logicalSize_ = frame.logicalSize;
	indexBuffer_ = frame.indexBuffer;
	palette_ = frame.palette;
	diagnosticTransformEnabled_ = frame.diagnosticTransform;
	renderLayerDiagnosticMode_ = frame.renderLayerDiagnosticMode;
	renderLayerMap_ = frame.renderLayerMap;
	worldMaskMap_ = frame.worldMaskMap;
	renderWorldMaskDiagnosticMode_ = frame.renderWorldMaskDiagnosticMode;
	worldProxyMap_ = frame.worldProxyMap;
	renderWorldProxyDiagnosticMode_ = frame.renderWorldProxyDiagnosticMode;
	compositionSurfaceMetadata_ = WithFullFrameCompositionSurfaceBounds(frame.compositionSurfaceMetadata, frame.logicalSize);

	lastCompositionStats_ = {};
	lastCompositionStats_.compositorEnabled = true;
	lastCompositionStats_.layerCaptureEnabled = frame.renderLayerMap.pixels != nullptr || frame.worldMaskMap.materialPixels != nullptr || frame.worldProxyMap.depthPixels != nullptr;
	RecordCompositionSurfaceStats(lastCompositionStats_, compositionSurfaceMetadata_);
	lastBackendResult_ = FrameCompositorBackendResult::NoFrameProduced;
	directPresentationPending_ = false;

	if (logicalSize_.width <= 0 || logicalSize_.height <= 0) {
		SetRenderPerfCompositionStats(lastCompositionStats_);
		return;
	}

	const bool paletteChanged = hasComposedFrame_ && palette_.version != lastComposedPaletteVersion_;
	const bool diagnosticTransformChanged = hasComposedFrame_ && diagnosticTransformEnabled_ != lastComposedDiagnosticTransformEnabled_;
	const bool renderLayerDiagnosticModeChanged = hasComposedFrame_ && renderLayerDiagnosticMode_ != lastRenderLayerDiagnosticMode_;
	const bool renderLayerDiagnosticsRequested = renderLayerDiagnosticMode_ != RenderLayerDiagnosticMode::Off;
	const bool renderWorldMaskDiagnosticModeChanged = hasComposedFrame_ && renderWorldMaskDiagnosticMode_ != lastRenderWorldMaskDiagnosticMode_;
	const bool renderWorldMaskDiagnosticsRequested = renderWorldMaskDiagnosticMode_ != RenderWorldMaskDiagnosticMode::Off;
	const bool renderWorldProxyDiagnosticModeChanged = hasComposedFrame_ && renderWorldProxyDiagnosticMode_ != lastRenderWorldProxyDiagnosticMode_;
	const bool renderWorldProxyDiagnosticsRequested = renderWorldProxyDiagnosticMode_ != RenderWorldProxyDiagnosticMode::Off;
	const bool lightShadowDiagnosticsRequested = *GetOptions().Experimental.renderLightShadowDiagnosticMode != RenderLightShadowDiagnosticMode::Off
	    && *GetOptions().Experimental.renderFrameCompositorBackend == RenderFrameCompositorBackend::SdlGpuPalette;
	const Size bounds {
		std::min({ logicalSize_.width, indexBuffer_.width, outputSurface_ != nullptr ? outputSurface_->w : 0 }),
		std::min({ logicalSize_.height, indexBuffer_.height, outputSurface_ != nullptr ? outputSurface_->h : 0 }),
	};
	NormalizedDirtyRects normalizedDirtyRects = NormalizeDirtyRects(frame.dirtyRects, bounds);
	lastCompositionStats_.submittedDirtyRectCount = normalizedDirtyRects.submittedRectCount;
	lastCompositionStats_.normalizedDirtyRectCount = normalizedDirtyRects.normalizedRectCount;
	lastCompositionStats_.submittedDirtyArea = normalizedDirtyRects.submittedArea;
	lastCompositionStats_.normalizedDirtyArea = normalizedDirtyRects.normalizedArea;

	const bool noDirtyRects = normalizedDirtyRects.dirtyRects.rects.empty() && !normalizedDirtyRects.dirtyRects.fullFrame;
	const bool emptyInitialFrame = !hasComposedFrame_ && noDirtyRects;
	CompositionFullFrameReason fullFrameReason = CompositionFullFrameReason::None;
	if (normalizedDirtyRects.dirtyRects.fullFrame) {
		fullFrameReason = normalizedDirtyRects.tooManyDirtyRects ? CompositionFullFrameReason::TooManyDirtyRects : CompositionFullFrameReason::Requested;
	} else if (paletteChanged) {
		fullFrameReason = CompositionFullFrameReason::PaletteChanged;
	} else if (diagnosticTransformChanged) {
		fullFrameReason = CompositionFullFrameReason::DiagnosticTransformChanged;
	} else if (renderLayerDiagnosticModeChanged) {
		fullFrameReason = CompositionFullFrameReason::RenderLayerDiagnosticModeChanged;
	} else if (renderLayerDiagnosticsRequested) {
		fullFrameReason = CompositionFullFrameReason::RenderLayerDiagnosticsRequested;
	} else if (renderWorldMaskDiagnosticModeChanged) {
		fullFrameReason = CompositionFullFrameReason::WorldMaskDiagnosticModeChanged;
	} else if (renderWorldMaskDiagnosticsRequested) {
		fullFrameReason = CompositionFullFrameReason::WorldMaskDiagnosticsRequested;
	} else if (renderWorldProxyDiagnosticModeChanged) {
		fullFrameReason = CompositionFullFrameReason::WorldProxyDiagnosticModeChanged;
	} else if (renderWorldProxyDiagnosticsRequested) {
		fullFrameReason = CompositionFullFrameReason::WorldProxyDiagnosticsRequested;
	} else if (lightShadowDiagnosticsRequested) {
		fullFrameReason = CompositionFullFrameReason::LightShadowDiagnosticRequested;
	} else if (outputSurfaceChanged) {
		fullFrameReason = CompositionFullFrameReason::OutputSurfaceChanged;
	} else if (indexBufferChanged) {
		fullFrameReason = CompositionFullFrameReason::IndexBufferChanged;
	} else if (logicalSizeChanged) {
		fullFrameReason = CompositionFullFrameReason::LogicalSizeChanged;
	} else if (noDirtyRects && lastComposedFrameUsedDirectPresentation_ && (backend_ == nullptr || !backend_->CanRetainDirectPresentation())) {
		fullFrameReason = CompositionFullFrameReason::DirectPresentationUnavailable;
	} else if (emptyInitialFrame) {
		fullFrameReason = CompositionFullFrameReason::FirstFrame;
	}

	const auto recordComposedRect = [&](Rectangle rect) {
		rect = ClipToBounds(rect, bounds);
		if (IsEmpty(rect))
			return;
		lastCompositionStats_.composedRectCount++;
		lastCompositionStats_.composedPixelArea += Area(rect);
	};
	const auto markComposedFrame = [&]() {
		hasComposedFrame_ = true;
		lastComposedPaletteVersion_ = palette_.version;
		lastComposedDiagnosticTransformEnabled_ = diagnosticTransformEnabled_;
		lastRenderLayerDiagnosticMode_ = renderLayerDiagnosticMode_;
		lastRenderWorldMaskDiagnosticMode_ = renderWorldMaskDiagnosticMode_;
		lastRenderWorldProxyDiagnosticMode_ = renderWorldProxyDiagnosticMode_;
		outputSurfaceChangedSinceComposition_ = false;
		indexBufferChangedSinceComposition_ = false;
		logicalSizeChangedSinceComposition_ = false;
	};
	const auto composeRects = [&](std::vector<Rectangle> rects) {
		CompositionFrame backendFrame {
			logicalSize_,
			indexBuffer_,
			palette_,
			frame.dirtyRects,
			diagnosticTransformEnabled_,
			renderLayerDiagnosticMode_,
			renderLayerMap_,
			compositionSurfaceMetadata_,
			frame.attachments,
			worldMaskMap_,
			renderWorldMaskDiagnosticMode_,
			worldProxyMap_,
			renderWorldProxyDiagnosticMode_,
		};
		UpsertCompositionAttachment(backendFrame.attachments, MakeIndexedAlbedoAttachment(indexBuffer_, logicalSize_, frame.dirtyRects));
		UpsertCompositionAttachment(backendFrame.attachments, MakePaletteAttachment(palette_));
		UpsertCompositionSurfaceRoleAttachments(backendFrame.attachments, compositionSurfaceMetadata_, indexBuffer_, logicalSize_);
		UpsertWorldMaskAttachments(backendFrame.attachments, worldMaskMap_, logicalSize_, frame.dirtyRects);
		UpsertWorldProxyAttachments(backendFrame.attachments, worldProxyMap_, logicalSize_, frame.dirtyRects);

		const FrameCompositorBackendResult result = ComposeRects(backendFrame, rects);
		RecordBackendResult(lastCompositionStats_, result);
		if (result == FrameCompositorBackendResult::NoFrameProduced) {
			return false;
		}
		lastBackendResult_ = result;
		directPresentationPending_ = UsesDirectPresentation(result);
		lastComposedFrameUsedDirectPresentation_ = directPresentationPending_;
		for (const Rectangle &rect : rects) {
			recordComposedRect(rect);
		}
		markComposedFrame();
		return true;
	};

	if (fullFrameReason != CompositionFullFrameReason::None) {
		lastCompositionStats_.fullFrameComposed = true;
		lastCompositionStats_.fullFrameReason = fullFrameReason;
		composeRects({ { { 0, 0 }, logicalSize_ } });
		SetRenderPerfCompositionStats(lastCompositionStats_);
		return;
	}

	if (noDirtyRects && backend_ != nullptr && backend_->CanRetainDirectPresentation()) {
		lastBackendResult_ = FrameCompositorBackendResult::RetainedDirectPresentation;
		RecordBackendResult(lastCompositionStats_, lastBackendResult_);
		directPresentationPending_ = true;
		lastComposedFrameUsedDirectPresentation_ = true;
		markComposedFrame();
		SetRenderPerfCompositionStats(lastCompositionStats_);
		return;
	}

	composeRects(normalizedDirtyRects.dirtyRects.rects);
	SetRenderPerfCompositionStats(lastCompositionStats_);
}

FrameCompositorBackendResult CpuPaletteCompositor::ComposeRects(const CompositionFrame &frame, const std::vector<Rectangle> &rects)
{
	if (rects.empty() || outputSurface_ == nullptr || backend_ == nullptr || !backend_->IsAvailable())
		return FrameCompositorBackendResult::NoFrameProduced;

	return backend_->Compose(frame, *outputSurface_, rects, lastCompositionStats_);
}

void CpuPaletteCompositor::Compose()
{
	CompositionFrame frame {
		logicalSize_,
		indexBuffer_,
		palette_,
		dirtyRects_,
		diagnosticTransformEnabled_,
		renderLayerDiagnosticMode_,
		renderLayerMap_,
		compositionSurfaceMetadata_,
		{},
		worldMaskMap_,
		renderWorldMaskDiagnosticMode_,
		worldProxyMap_,
		renderWorldProxyDiagnosticMode_,
	};
	UpsertCompositionAttachment(frame.attachments, MakeIndexedAlbedoAttachment(indexBuffer_, logicalSize_, dirtyRects_));
	UpsertCompositionAttachment(frame.attachments, MakePaletteAttachment(palette_));
	UpsertCompositionSurfaceRoleAttachments(frame.attachments, compositionSurfaceMetadata_, indexBuffer_, logicalSize_);
	UpsertWorldMaskAttachments(frame.attachments, worldMaskMap_, logicalSize_, dirtyRects_);
	UpsertWorldProxyAttachments(frame.attachments, worldProxyMap_, logicalSize_, dirtyRects_);
	Compose(frame);
}

void CpuPaletteCompositor::Present()
{
	if (backend_ != nullptr && directPresentationPending_)
		backend_->Present();
	directPresentationPending_ = false;
	ResetDirtyRects();
}

bool FrameCompositionEnabled()
{
	return *GetOptions().Experimental.renderFrameCompositor;
}

void SubmitFrameCompositionDirtyRect(Rectangle rect)
{
	const RenderLayer layer = CurrentRenderLayer();
	RecordRenderLayerDirtyRect(layer);
	FrameCompositor.AddDirtyRect(rect, CompositionSurfaceRoleForRenderLayer(layer));
}

void SubmitFrameCompositionFullFrame()
{
	const RenderLayer layer = CurrentRenderLayer();
	RecordRenderLayerDirtyRect(layer);
	FrameCompositor.SetFullFrameDirty(CompositionSurfaceRoleForRenderLayer(layer));
}

void ResetFrameCompositionDirtyRects()
{
	FrameCompositor.ResetDirtyRects();
}

bool ComposeFrameToOutput(SDL_Surface *outputSurface)
{
	if (!FrameCompositionEnabled()) {
		SetRenderPerfCompositionStats({});
		return false;
	}

	EnsureFrameCompositorBackend();
	FrameCompositor.SetOutputSurface(outputSurface);
	{
		RenderPerfScope renderPerfScope(RenderPerfPhase::Compose);
		FrameCompositor.Compose({
		    { gnScreenWidth, gnScreenHeight },
		    MakeIndexBufferView(*PalSurface),
		    MakePaletteSnapshot(system_palette, SystemPaletteVersion()),
		    FrameCompositor.GetDirtyRects(),
		    *GetOptions().Experimental.renderFrameCompositorDiagnosticTransform,
		    *GetOptions().Experimental.renderLayerDiagnosticMode,
		    CurrentRenderLayerMapView(),
		    FrameCompositor.GetCompositionSurfaceMetadata(),
		    {},
		    CurrentRenderWorldMaskMapView(),
		    *GetOptions().Experimental.renderWorldMaskDiagnosticMode,
		    CurrentRenderWorldProxyMapView(),
		    *GetOptions().Experimental.renderWorldProxyDiagnosticMode,
		});
	}
	const FrameCompositorBackendResult backendResult = FrameCompositor.GetLastBackendResult();
	return UsesDirectPresentation(backendResult);
}

void PresentFrameComposition()
{
	if (!FrameCompositionEnabled())
		return;
	FrameCompositor.Present();
}

void ShutdownFrameComposition()
{
	FrameCompositor.SetBackend(CreateCpuFrameCompositorBackend());
	CurrentFrameCompositorBackendInitialized = false;
	CurrentFrameCompositorBackend = RenderFrameCompositorBackend::CpuPalette;
}

} // namespace devilution

#undef DEVILUTIONX_PARALLEL_COMPOSITION
#undef DEVILUTIONX_LEGACY_WINDOWS_9X
