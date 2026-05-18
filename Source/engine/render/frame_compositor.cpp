/**
 * @file frame_compositor.cpp
 *
 * Final frame composition helpers for the indexed renderer.
 */
#include "engine/render/frame_compositor.hpp"

#include <algorithm>
#include <cstring>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "engine/dx.h"
#include "engine/palette.h"
#include "options.h"
#include "appfat.h"
#include "utils/display.h"
#include "utils/sdl_compat.h"

namespace devilution {
namespace {

CpuPaletteCompositor FrameCompositor;

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

void PutPixel(SDL_Surface &surface, const int x, const int y, const uint32_t pixel)
{
	uint8_t *dst = static_cast<uint8_t *>(surface.pixels) + y * surface.pitch + x * BytesPerPixel(surface);
	switch (BytesPerPixel(surface)) {
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

void CpuPaletteCompositor::BeginFrame(const Size logicalSize)
{
	logicalSize_ = logicalSize;
}

void CpuPaletteCompositor::SubmitIndexBuffer(const IndexBufferView indexBuffer)
{
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

void CpuPaletteCompositor::ComposeRect(Rectangle rect)
{
	if (outputSurface_ == nullptr || indexBuffer_.pixels == nullptr)
		return;

	const Size bounds {
		std::min({ logicalSize_.width, indexBuffer_.width, outputSurface_->w }),
		std::min({ logicalSize_.height, indexBuffer_.height, outputSurface_->h }),
	};
	rect = ClipToBounds(rect, bounds);
	if (rect.size.width == 0 || rect.size.height == 0)
		return;

	const bool mustLock = SDL_MUSTLOCK(outputSurface_);
	if (mustLock) {
#ifdef USE_SDL3
		if (!SDL_LockSurface(outputSurface_)) ErrSdl();
#else
		if (SDL_LockSurface(outputSurface_) < 0) ErrSdl();
#endif
	}

	std::array<uint32_t, 256> mappedPalette;
	for (size_t i = 0; i < mappedPalette.size(); i++) {
		const RgbColor color = diagnosticTransformEnabled_ ? ApplyDiagnosticTransform(palette_.colors[i]) : palette_.colors[i];
		mappedPalette[i] = MapRgba(*outputSurface_, color);
	}

	for (int y = rect.position.y; y < rect.position.y + rect.size.height; y++) {
		const uint8_t *src = indexBuffer_.pixels + y * indexBuffer_.pitch + rect.position.x;
		for (int x = 0; x < rect.size.width; x++) {
			PutPixel(*outputSurface_, rect.position.x + x, y, mappedPalette[src[x]]);
		}
	}

	if (mustLock)
		SDL_UnlockSurface(outputSurface_);
}

void CpuPaletteCompositor::Compose()
{
	if (logicalSize_.width <= 0 || logicalSize_.height <= 0)
		return;

	if (dirtyRects_.fullFrame || dirtyRects_.rects.empty()) {
		ComposeRect({ { 0, 0 }, logicalSize_ });
		return;
	}

	for (const Rectangle &rect : dirtyRects_.rects) {
		ComposeRect(rect);
	}
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
	FrameCompositor.AddDirtyRect(rect);
}

void SubmitFrameCompositionFullFrame()
{
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

	FrameCompositor.BeginFrame({ gnScreenWidth, gnScreenHeight });
	FrameCompositor.SubmitIndexBuffer(MakeIndexBufferView(*PalSurface));
	FrameCompositor.SubmitPalette(MakePaletteSnapshot(system_palette, SystemPaletteVersion()));
	FrameCompositor.SetOutputSurface(outputSurface);
	FrameCompositor.SetDiagnosticTransformEnabled(*GetOptions().Experimental.renderFrameCompositorDiagnosticTransform);
	FrameCompositor.Compose();
	FrameCompositor.Present();
}

} // namespace devilution
