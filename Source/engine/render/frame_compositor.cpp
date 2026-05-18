/**
 * @file frame_compositor.cpp
 *
 * Final frame composition helpers for the indexed renderer.
 */
#include "engine/render/frame_compositor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#if !defined(__DJGPP__) && !defined(__EMSCRIPTEN__) && !defined(__amigaos__)
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

#if !defined(__DJGPP__) && !defined(__EMSCRIPTEN__) && !defined(__amigaos__)
#define DEVILUTIONX_PARALLEL_COMPOSITION 1
#else
#define DEVILUTIONX_PARALLEL_COMPOSITION 0
#endif

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

[[nodiscard]] int BytesPerPixel(const SDL_Surface &surface)
{
#ifdef USE_SDL3
	return SDL_BYTESPERPIXEL(surface.format);
#else
	return surface.format->BytesPerPixel;
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
	if (ThreadCountOverrideForTesting > 0)
		return std::min(ThreadCountOverrideForTesting, std::max(1, rect.size.height));
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

[[nodiscard]] DirtyRectList NormalizeDirtyRects(const DirtyRectList &dirtyRects, const Size bounds)
{
	DirtyRectList normalized;
	if (dirtyRects.fullFrame) {
		normalized.fullFrame = true;
		return normalized;
	}

	for (Rectangle rect : dirtyRects.rects) {
		if (IsEmpty(rect))
			continue;

		rect = ClipToBounds(rect, bounds);
		if (IsEmpty(rect))
			continue;

		AddNormalizedDirtyRect(normalized, rect);
		if (normalized.rects.size() > MaxDirtyRectsBeforeFullFrame) {
			normalized.rects.clear();
			normalized.fullFrame = true;
			return normalized;
		}
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
	if (logicalSize_ != logicalSize)
		hasComposedFrame_ = false;
	logicalSize_ = logicalSize;
}

void CpuPaletteCompositor::SubmitIndexBuffer(const IndexBufferView indexBuffer)
{
	if (IndexBufferIdentityChanged(indexBuffer_, indexBuffer))
		hasComposedFrame_ = false;
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
	if (outputSurface_ != outputSurface)
		hasComposedFrame_ = false;
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
	std::array<uint32_t, 256> mappedPalette;
	std::array<std::array<uint32_t, 256>, RenderLayerDiagnosticColorCount> mappedTintPalettes {};
	std::array<uint32_t, RenderLayerDiagnosticColorCount> mappedOutlineColors {};
	for (size_t i = 0; i < mappedPalette.size(); i++) {
		const RgbColor color = diagnosticTransformEnabled_ ? ApplyDiagnosticTransform(palette_.colors[i]) : palette_.colors[i];
		mappedPalette[i] = MapRgba(*outputSurface_, color);
		if (renderLayerTintEnabled) {
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
	const auto composeRows = [&](const int yBegin, const int yEnd) {
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

	const int threadCount = CompositionThreadCount(rect);
#if DEVILUTIONX_PARALLEL_COMPOSITION
	if (threadCount == 1) {
		composeRows(rect.position.y, rect.position.y + rect.size.height);
	} else {
		std::vector<std::thread> workers;
		workers.reserve(static_cast<size_t>(threadCount - 1));
		const int rowsPerThread = rect.size.height / threadCount;
		const int extraRows = rect.size.height % threadCount;
		int yBegin = rect.position.y;
		for (int i = 0; i < threadCount - 1; i++) {
			const int bandHeight = rowsPerThread + (i < extraRows ? 1 : 0);
			const int yEnd = yBegin + bandHeight;
			workers.emplace_back(composeRows, yBegin, yEnd);
			yBegin = yEnd;
		}
		composeRows(yBegin, rect.position.y + rect.size.height);
		for (std::thread &worker : workers) {
			worker.join();
		}
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
	if (logicalSize_ != frame.logicalSize)
		hasComposedFrame_ = false;
	if (IndexBufferIdentityChanged(indexBuffer_, frame.indexBuffer))
		hasComposedFrame_ = false;
	logicalSize_ = frame.logicalSize;
	indexBuffer_ = frame.indexBuffer;
	palette_ = frame.palette;
	diagnosticTransformEnabled_ = frame.diagnosticTransform;
	renderLayerDiagnosticMode_ = frame.renderLayerDiagnosticMode;
	renderLayerMap_ = frame.renderLayerMap;

	if (logicalSize_.width <= 0 || logicalSize_.height <= 0)
		return;

	const bool paletteChanged = hasComposedFrame_ && palette_.version != lastComposedPaletteVersion_;
	const bool diagnosticTransformChanged = hasComposedFrame_ && diagnosticTransformEnabled_ != lastComposedDiagnosticTransformEnabled_;
	const bool renderLayerDiagnosticModeChanged = hasComposedFrame_ && renderLayerDiagnosticMode_ != lastRenderLayerDiagnosticMode_;
	const bool renderLayerDiagnosticsRequested = renderLayerDiagnosticMode_ != RenderLayerDiagnosticMode::Off;
	const Size bounds {
		std::min({ logicalSize_.width, indexBuffer_.width, outputSurface_ != nullptr ? outputSurface_->w : 0 }),
		std::min({ logicalSize_.height, indexBuffer_.height, outputSurface_ != nullptr ? outputSurface_->h : 0 }),
	};
	DirtyRectList normalizedDirtyRects = NormalizeDirtyRects(frame.dirtyRects, bounds);
	const bool emptyInitialFrame = !hasComposedFrame_ && normalizedDirtyRects.rects.empty() && !normalizedDirtyRects.fullFrame;
	if (normalizedDirtyRects.fullFrame || paletteChanged || diagnosticTransformChanged || renderLayerDiagnosticModeChanged || renderLayerDiagnosticsRequested || emptyInitialFrame) {
		if (ComposeRect({ { 0, 0 }, logicalSize_ })) {
			hasComposedFrame_ = true;
			lastComposedPaletteVersion_ = palette_.version;
			lastComposedDiagnosticTransformEnabled_ = diagnosticTransformEnabled_;
			lastRenderLayerDiagnosticMode_ = renderLayerDiagnosticMode_;
		}
		return;
	}

	bool composed = false;
	for (const Rectangle &rect : normalizedDirtyRects.rects) {
		composed = ComposeRect(rect) || composed;
	}
	if (composed) {
		hasComposedFrame_ = true;
		lastComposedPaletteVersion_ = palette_.version;
		lastComposedDiagnosticTransformEnabled_ = diagnosticTransformEnabled_;
		lastRenderLayerDiagnosticMode_ = renderLayerDiagnosticMode_;
	}
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
	if (!FrameCompositionEnabled())
		return;

	FrameCompositor.SetOutputSurface(outputSurface);
	FrameCompositor.Compose({
	    { gnScreenWidth, gnScreenHeight },
	    MakeIndexBufferView(*PalSurface),
	    MakePaletteSnapshot(system_palette, SystemPaletteVersion()),
	    FrameCompositor.GetDirtyRects(),
	    *GetOptions().Experimental.renderFrameCompositorDiagnosticTransform,
	    *GetOptions().Experimental.renderLayerDiagnosticMode,
	    CurrentRenderLayerMapView(),
	});
	FrameCompositor.Present();
}

} // namespace devilution

#undef DEVILUTIONX_PARALLEL_COMPOSITION
