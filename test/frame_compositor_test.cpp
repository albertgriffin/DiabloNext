#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "engine/render/frame_compositor.hpp"
#include "utils/sdl_wrap.h"

namespace devilution {
namespace {

uint32_t ReadPixel(const SDL_Surface &surface, const int x, const int y)
{
#ifdef USE_SDL3
	const int bytesPerPixel = SDL_BYTESPERPIXEL(surface.format);
#else
	const int bytesPerPixel = surface.format->BytesPerPixel;
#endif
	const uint8_t *src = static_cast<const uint8_t *>(surface.pixels) + y * surface.pitch + x * bytesPerPixel;
	uint32_t pixel = 0;
	std::memcpy(&pixel, src, bytesPerPixel);
	return pixel;
}

SDL_Color ReadColor(const SDL_Surface &surface, const int x, const int y)
{
	SDL_Color color {};
	const uint32_t pixel = ReadPixel(surface, x, y);
#ifdef USE_SDL3
	SDL_GetRGBA(pixel, SDL_GetPixelFormatDetails(surface.format), SDL_GetSurfacePalette(const_cast<SDL_Surface *>(&surface)), &color.r, &color.g, &color.b, &color.a);
#else
	SDL_GetRGBA(pixel, surface.format, &color.r, &color.g, &color.b, &color.a);
#endif
	return color;
}

TEST(FrameCompositor, MakesIndexBufferViewFromEightBitSurface)
{
	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 2, 8, SDL_PIXELFORMAT_INDEX8);

	IndexBufferView view = MakeIndexBufferView(*surface);

	EXPECT_EQ(view.pixels, static_cast<uint8_t *>(surface->pixels));
	EXPECT_EQ(view.width, 3);
	EXPECT_EQ(view.height, 2);
	EXPECT_EQ(view.pitch, surface->pitch);
}

TEST(FrameCompositor, CpuPaletteCompositorExpandsPaletteIndices)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 2, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 2;
	pixels[indexSurface->pitch] = 3;
	pixels[indexSurface->pitch + 1] = 4;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 255, 0, 0, 255 };
	palette[2] = { 0, 255, 0, 255 };
	palette[3] = { 0, 0, 255, 255 };
	palette[4] = { 255, 255, 255, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 2, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 2, 2 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 7));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).g, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 0, 1).b, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 1).r, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 1).g, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 1).b, 255);
}

TEST(FrameCompositor, CpuPaletteCompositorRespectsDirtyRects)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 2;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 20, 30, 255 };
	palette[2] = { 200, 210, 220, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 2, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 8));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.AddDirtyRect({ { 1, 0 }, { 1, 1 } });
	compositor.Compose();

	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 0);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).r, 200);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).g, 210);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).b, 220);
}

TEST(FrameCompositor, CpuPaletteCompositorAppliesDiagnosticTransformAfterPaletteExpansion)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 100, 150, 200, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 9));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.SetDiagnosticTransformEnabled(true);
	compositor.SetFullFrameDirty();
	compositor.Compose();

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_NE(color.r, 100);
	EXPECT_NE(color.g, 150);
	EXPECT_NE(color.b, 200);
}

} // namespace
} // namespace devilution
