/**
 * @file frame_compositor.cpp
 *
 * Final frame composition helpers for the indexed renderer.
 */
#include "engine/render/frame_compositor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

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
#if DEVILUTIONX_PARALLEL_COMPOSITION
constexpr int MinParallelCompositionPixels = 96 * 1024;
constexpr int MinParallelCompositionRowsPerThread = 64;
constexpr unsigned MaxParallelCompositionThreads = 6;
#endif
#ifdef BUILD_TESTING
int ThreadCountOverrideForTesting = 0;
#endif

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

#ifdef BUILD_TESTING
void SetFrameCompositorThreadCountOverrideForTesting(const int threadCount)
{
	ThreadCountOverrideForTesting = std::max(0, threadCount);
}
#endif

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
	if (rect.size.width <= 0 || rect.size.height <= 0)
		return;
	dirtyRects_.rects.push_back(rect);
}

void CpuPaletteCompositor::SetFullFrameDirty()
{
	dirtyRects_.fullFrame = true;
}

void CpuPaletteCompositor::ResetDirtyRects()
{
	dirtyRects_ = {};
}

void CpuPaletteCompositor::SetDiagnosticTransformEnabled(const bool enabled)
{
	diagnosticTransformEnabled_ = enabled;
}

const DirtyRectList &CpuPaletteCompositor::GetDirtyRects() const
{
	return dirtyRects_;
}

const RenderPerfCompositionStats &CpuPaletteCompositor::GetLastCompositionStats() const
{
	return lastCompositionStats_;
}

const std::array<uint32_t, 256> &CpuPaletteCompositor::GetMappedPalette(const bool diagnosticTransform)
{
	if (outputSurface_ == nullptr)
		return mappedPaletteCache_.mappedPalette;

	const uintptr_t outputFormatIdentity = OutputSurfaceFormatIdentity(*outputSurface_);
	if (mappedPaletteCache_.valid
	    && mappedPaletteCache_.outputSurface == outputSurface_
	    && mappedPaletteCache_.outputFormatIdentity == outputFormatIdentity
	    && mappedPaletteCache_.paletteVersion == palette_.version
	    && mappedPaletteCache_.diagnosticTransform == diagnosticTransform) {
		return mappedPaletteCache_.mappedPalette;
	}

	for (size_t i = 0; i < mappedPaletteCache_.mappedPalette.size(); i++) {
		const RgbColor color = diagnosticTransform ? ApplyDiagnosticTransform(palette_.colors[i]) : palette_.colors[i];
		mappedPaletteCache_.mappedPalette[i] = MapRgba(*outputSurface_, color);
	}
	mappedPaletteCache_.valid = true;
	mappedPaletteCache_.outputSurface = outputSurface_;
	mappedPaletteCache_.outputFormatIdentity = outputFormatIdentity;
	mappedPaletteCache_.paletteVersion = palette_.version;
	mappedPaletteCache_.diagnosticTransform = diagnosticTransform;
	return mappedPaletteCache_.mappedPalette;
}

bool CpuPaletteCompositor::ComposeRect(Rectangle rect)
{
	if (outputSurface_ == nullptr || indexBuffer_.pixels == nullptr)
		return false;

	const Size bounds {
		std::min({ logicalSize_.width, indexBuffer_.width, outputSurface_->w }),
		std::min({ logicalSize_.height, indexBuffer_.height, outputSurface_->h }),
	};
	rect = ClipToBounds(rect, bounds);
	if (rect.size.width == 0 || rect.size.height == 0)
		return false;

	const bool mustLock = SDL_MUSTLOCK(outputSurface_);
	if (mustLock) {
#ifdef USE_SDL3
		if (!SDL_LockSurface(outputSurface_)) ErrSdl();
#else
		if (SDL_LockSurface(outputSurface_) < 0) ErrSdl();
#endif
	}

	const bool renderLayerDiagnosticsEnabled = renderLayerDiagnosticMode_ != RenderLayerDiagnosticMode::Off && renderLayerMap_.pixels != nullptr;
	const bool renderLayerTintEnabled = renderLayerDiagnosticsEnabled && UsesRenderLayerTint(renderLayerDiagnosticMode_);
	const bool renderLayerOutlineEnabled = renderLayerDiagnosticsEnabled && UsesRenderLayerOutline(renderLayerDiagnosticMode_);
	const std::array<uint32_t, 256> &mappedPalette = GetMappedPalette(diagnosticTransformEnabled_);
	std::array<std::array<uint32_t, 256>, RenderLayerDiagnosticColorCount> mappedTintPalettes {};
	std::array<uint32_t, RenderLayerDiagnosticColorCount> mappedOutlineColors {};
	if (renderLayerTintEnabled) {
		for (size_t i = 0; i < mappedPalette.size(); i++) {
			const RgbColor color = diagnosticTransformEnabled_ ? ApplyDiagnosticTransform(palette_.colors[i]) : palette_.colors[i];
			for (size_t colorIndex = 0; colorIndex < mappedTintPalettes.size(); colorIndex++) {
				RgbColor tintedColor = color;
				const RgbColor layerColor = RenderLayerDiagnosticColorByIndex(colorIndex);
				tintedColor.r = static_cast<uint8_t>((static_cast<uint16_t>(tintedColor.r) + layerColor.r) / 2);
				tintedColor.g = static_cast<uint8_t>((static_cast<uint16_t>(tintedColor.g) + layerColor.g) / 2);
				tintedColor.b = static_cast<uint8_t>((static_cast<uint16_t>(tintedColor.b) + layerColor.b) / 2);
				mappedTintPalettes[colorIndex][i] = MapRgba(*outputSurface_, tintedColor);
			}
		}
	}
	if (renderLayerOutlineEnabled) {
		for (size_t colorIndex = 0; colorIndex < mappedOutlineColors.size(); colorIndex++)
			mappedOutlineColors[colorIndex] = MapRgba(*outputSurface_, RenderLayerDiagnosticColorByIndex(colorIndex));
	}

	const int bytesPerPixel = BytesPerPixel(*outputSurface_);
	const bool useFast32NoDiagnostics = bytesPerPixel == 4 && !diagnosticTransformEnabled_ && !renderLayerDiagnosticsEnabled;
	const auto composeRows32NoDiagnostics = [&](const int yBegin, const int yEnd) {
		for (int y = yBegin; y < yEnd; y++) {
			const uint8_t *src = indexBuffer_.pixels + y * indexBuffer_.pitch + rect.position.x;
			uint32_t *dst = reinterpret_cast<uint32_t *>(
			    static_cast<uint8_t *>(outputSurface_->pixels) + static_cast<ptrdiff_t>(y) * outputSurface_->pitch + rect.position.x * sizeof(uint32_t));
			for (int x = 0; x < rect.size.width; x++) {
				dst[x] = mappedPalette[src[x]];
			}
		}
	};
	const auto composeRowsGeneric = [&](const int yBegin, const int yEnd) {
		for (int y = yBegin; y < yEnd; y++) {
			const uint8_t *src = indexBuffer_.pixels + y * indexBuffer_.pitch + rect.position.x;
			uint8_t *dstRow = static_cast<uint8_t *>(outputSurface_->pixels) + static_cast<ptrdiff_t>(y) * outputSurface_->pitch + rect.position.x * bytesPerPixel;
			uint32_t *dst32 = bytesPerPixel == 4 ? reinterpret_cast<uint32_t *>(dstRow) : nullptr;
			const bool layerRowInBounds = renderLayerMap_.pixels != nullptr && y >= 0 && y < renderLayerMap_.height;
			const uint8_t *layerRow = layerRowInBounds ? renderLayerMap_.pixels + static_cast<size_t>(y) * renderLayerMap_.pitch : nullptr;
			const uint8_t *layerRowAbove = renderLayerMap_.pixels != nullptr && y > 0 && y - 1 < renderLayerMap_.height ? renderLayerMap_.pixels + static_cast<size_t>(y - 1) * renderLayerMap_.pitch : nullptr;
			const uint8_t *layerRowBelow = renderLayerMap_.pixels != nullptr && y + 1 < renderLayerMap_.height ? renderLayerMap_.pixels + static_cast<size_t>(y + 1) * renderLayerMap_.pitch : nullptr;
			for (int x = 0; x < rect.size.width; x++) {
				const int outputX = rect.position.x + x;
				uint32_t pixel = mappedPalette[src[x]];
				if (renderLayerDiagnosticsEnabled) {
					uint8_t layerId = UnknownRenderLayerId;
					if (layerRow != nullptr && outputX >= 0 && outputX < renderLayerMap_.width)
						layerId = layerRow[outputX];
					const size_t colorIndex = RenderLayerDiagnosticColorIndex(layerId);
					if (renderLayerTintEnabled)
						pixel = mappedTintPalettes[colorIndex][src[x]];
					if (renderLayerOutlineEnabled) {
						bool isBoundary = layerId == UnknownRenderLayerId;
						if (!isBoundary) {
							if (outputX > 0 && layerRow[outputX - 1] != layerId)
								isBoundary = true;
							else if (outputX + 1 < renderLayerMap_.width && layerRow[outputX + 1] != layerId)
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
	lastCompositionStats_.selectedThreadCount = std::max(lastCompositionStats_.selectedThreadCount, threadCount);
#if DEVILUTIONX_PARALLEL_COMPOSITION
	if (threadCount > 1)
		lastCompositionStats_.parallelCompositionUsed = true;
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
		SDL_UnlockSurface(outputSurface_);
	return true;
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

	lastCompositionStats_ = {};
	lastCompositionStats_.compositorEnabled = true;
	lastCompositionStats_.layerCaptureEnabled = frame.renderLayerMap.pixels != nullptr;

	if (logicalSize_.width <= 0 || logicalSize_.height <= 0) {
		SetRenderPerfCompositionStats(lastCompositionStats_);
		return;
	}

	const bool paletteChanged = hasComposedFrame_ && palette_.version != lastComposedPaletteVersion_;
	const bool diagnosticTransformChanged = hasComposedFrame_ && diagnosticTransformEnabled_ != lastComposedDiagnosticTransformEnabled_;
	const bool renderLayerDiagnosticModeChanged = hasComposedFrame_ && renderLayerDiagnosticMode_ != lastRenderLayerDiagnosticMode_;
	const bool renderLayerDiagnosticsRequested = renderLayerDiagnosticMode_ != RenderLayerDiagnosticMode::Off;
	const Size bounds {
		std::min({ logicalSize_.width, indexBuffer_.width, outputSurface_ != nullptr ? outputSurface_->w : 0 }),
		std::min({ logicalSize_.height, indexBuffer_.height, outputSurface_ != nullptr ? outputSurface_->h : 0 }),
	};
	NormalizedDirtyRects normalizedDirtyRects = NormalizeDirtyRects(frame.dirtyRects, bounds);
	lastCompositionStats_.submittedDirtyRectCount = normalizedDirtyRects.submittedRectCount;
	lastCompositionStats_.normalizedDirtyRectCount = normalizedDirtyRects.normalizedRectCount;
	lastCompositionStats_.submittedDirtyArea = normalizedDirtyRects.submittedArea;
	lastCompositionStats_.normalizedDirtyArea = normalizedDirtyRects.normalizedArea;

	const bool emptyInitialFrame = !hasComposedFrame_ && normalizedDirtyRects.dirtyRects.rects.empty() && !normalizedDirtyRects.dirtyRects.fullFrame;
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
	} else if (outputSurfaceChanged) {
		fullFrameReason = CompositionFullFrameReason::OutputSurfaceChanged;
	} else if (indexBufferChanged) {
		fullFrameReason = CompositionFullFrameReason::IndexBufferChanged;
	} else if (logicalSizeChanged) {
		fullFrameReason = CompositionFullFrameReason::LogicalSizeChanged;
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
		outputSurfaceChangedSinceComposition_ = false;
		indexBufferChangedSinceComposition_ = false;
		logicalSizeChangedSinceComposition_ = false;
	};

	if (fullFrameReason != CompositionFullFrameReason::None) {
		lastCompositionStats_.fullFrameComposed = true;
		lastCompositionStats_.fullFrameReason = fullFrameReason;
		if (ComposeRect({ { 0, 0 }, logicalSize_ })) {
			recordComposedRect({ { 0, 0 }, logicalSize_ });
			markComposedFrame();
		}
		SetRenderPerfCompositionStats(lastCompositionStats_);
		return;
	}

	bool composed = false;
	for (const Rectangle &rect : normalizedDirtyRects.dirtyRects.rects) {
		if (ComposeRect(rect)) {
			recordComposedRect(rect);
			composed = true;
		}
	}
	if (composed) {
		markComposedFrame();
	}
	SetRenderPerfCompositionStats(lastCompositionStats_);
}

void CpuPaletteCompositor::Compose()
{
	Compose({
	    logicalSize_,
	    indexBuffer_,
	    palette_,
	    dirtyRects_,
	    diagnosticTransformEnabled_,
	    renderLayerDiagnosticMode_,
	    renderLayerMap_,
	});
}

void CpuPaletteCompositor::Present()
{
	ResetDirtyRects();
}

bool FrameCompositionEnabled()
{
	return *GetOptions().Experimental.renderFrameCompositor;
}

void SubmitFrameCompositionDirtyRect(Rectangle rect)
{
	RecordRenderLayerDirtyRect(CurrentRenderLayer());
	FrameCompositor.AddDirtyRect(rect);
}

void SubmitFrameCompositionFullFrame()
{
	RecordRenderLayerDirtyRect(CurrentRenderLayer());
	FrameCompositor.SetFullFrameDirty();
}

void ResetFrameCompositionDirtyRects()
{
	FrameCompositor.ResetDirtyRects();
}

void ComposeFrameToOutput(SDL_Surface *outputSurface)
{
	if (!FrameCompositionEnabled()) {
		SetRenderPerfCompositionStats({});
		return;
	}

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
		});
	}
	FrameCompositor.Present();
}

} // namespace devilution

#undef DEVILUTIONX_PARALLEL_COMPOSITION
#undef DEVILUTIONX_LEGACY_WINDOWS_9X
