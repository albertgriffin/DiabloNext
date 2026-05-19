#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "engine/render/frame_compositor.hpp"
#include "engine/render/render_layer.hpp"
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

class FrameCompositorThreadCountOverrideGuard {
public:
	explicit FrameCompositorThreadCountOverrideGuard(const int threadCount)
	{
		SetFrameCompositorThreadCountOverrideForTesting(threadCount);
	}

	~FrameCompositorThreadCountOverrideGuard()
	{
		SetFrameCompositorThreadCountOverrideForTesting(0);
	}
};

void ExpectWorldTint(const SDL_Surface &surface, const int x, const int y, const SDL_Color baseColor)
{
	const SDL_Color color = ReadColor(surface, x, y);
	EXPECT_EQ(color.r, baseColor.r / 2);
	EXPECT_EQ(color.g, (static_cast<uint16_t>(baseColor.g) + 255) / 2);
	EXPECT_EQ(color.b, baseColor.b / 2);
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

	const RenderPerfCompositionStats &stats = compositor.GetLastCompositionStats();
	EXPECT_FALSE(stats.fullFrameComposed);
	EXPECT_EQ(stats.fullFrameReason, CompositionFullFrameReason::None);
	EXPECT_EQ(stats.submittedDirtyRectCount, 1);
	EXPECT_EQ(stats.normalizedDirtyRectCount, 1);
	EXPECT_EQ(stats.composedRectCount, 1);
	EXPECT_EQ(stats.submittedDirtyArea, 1);
	EXPECT_EQ(stats.normalizedDirtyArea, 1);
	EXPECT_EQ(stats.composedPixelArea, 1);
}

TEST(FrameCompositor, CpuPaletteCompositorRecomposesFullFrameWhenPaletteVersionChanges)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 255, 0, 0, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose();
	compositor.Present();
	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 255);

	palette[1] = { 0, 255, 0, 255 };
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 2));
	compositor.Compose();

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 0);
	EXPECT_EQ(color.g, 255);

	const RenderPerfCompositionStats &stats = compositor.GetLastCompositionStats();
	EXPECT_TRUE(stats.fullFrameComposed);
	EXPECT_EQ(stats.fullFrameReason, CompositionFullFrameReason::PaletteChanged);
	EXPECT_EQ(stats.composedRectCount, 1);
	EXPECT_EQ(stats.composedPixelArea, 1);
}

TEST(FrameCompositor, CpuPaletteCompositorInvalidatesMappedPaletteWhenOutputFormatChanges)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 10, 80, 200, 255 };

	SDLSurfaceUniquePtr outputRgba = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);
	SDLSurfaceUniquePtr outputBgra = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_BGRA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputRgba.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();
	compositor.Present();

	SDL_Color color = ReadColor(*outputRgba, 0, 0);
	EXPECT_EQ(color.r, 10);
	EXPECT_EQ(color.g, 80);
	EXPECT_EQ(color.b, 200);

	compositor.BeginFrame({ 1, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputBgra.get());
	compositor.SetFullFrameDirty();
	compositor.Compose();

	color = ReadColor(*outputBgra, 0, 0);
	EXPECT_EQ(color.r, 10);
	EXPECT_EQ(color.g, 80);
	EXPECT_EQ(color.b, 200);
}

TEST(FrameCompositor, CpuPaletteCompositorTreatsEmptyDirtyRectsAsInitialFullFrameOnly)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 255, 0, 0, 255 };
	palette[2] = { 0, 255, 0, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 2, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose();
	compositor.Present();
	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 255);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).r, 255);

	pixels[0] = 2;
	compositor.BeginFrame({ 2, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.Compose();

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 255);
	EXPECT_EQ(color.g, 0);
}

TEST(FrameCompositor, CpuPaletteCompositorClipsDirtyRectsToOutputBounds)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;
	pixels[2] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 90, 10, 20, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ 3, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputSurface.get());
	compositor.AddDirtyRect({ { -1, 0 }, { 2, 1 } });
	compositor.Compose();

	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 90);
	EXPECT_EQ(ReadColor(*outputSurface, 1, 0).r, 0);
	EXPECT_EQ(ReadColor(*outputSurface, 2, 0).r, 0);
}

TEST(FrameCompositor, CpuPaletteCompositorEscalatesManyDirtyRectsToFullFrame)
{
	constexpr int Width = 70;
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, Width, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	for (int x = 0; x < Width; x++) {
		pixels[x] = 1;
	}

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 40, 50, 60, 255 };

	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, Width, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.BeginFrame({ Width, 1 });
	compositor.SubmitIndexBuffer(MakeIndexBufferView(*indexSurface));
	compositor.SubmitPalette(MakePaletteSnapshot(palette, 1));
	compositor.SetOutputSurface(outputSurface.get());
	for (int x = 0; x < 65; x++) {
		compositor.AddDirtyRect({ { x, 0 }, { 1, 1 } });
	}
	compositor.Compose();

	EXPECT_EQ(ReadColor(*outputSurface, 0, 0).r, 40);
	EXPECT_EQ(ReadColor(*outputSurface, Width - 1, 0).r, 40);

	const RenderPerfCompositionStats &stats = compositor.GetLastCompositionStats();
	EXPECT_TRUE(stats.fullFrameComposed);
	EXPECT_EQ(stats.fullFrameReason, CompositionFullFrameReason::TooManyDirtyRects);
	EXPECT_EQ(stats.submittedDirtyRectCount, 65);
	EXPECT_EQ(stats.normalizedDirtyRectCount, 0);
	EXPECT_EQ(stats.submittedDirtyArea, 65);
	EXPECT_EQ(stats.normalizedDirtyArea, Width);
	EXPECT_EQ(stats.composedRectCount, 1);
	EXPECT_EQ(stats.composedPixelArea, Width);
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

TEST(FrameCompositor, RenderLayerDiagnosticOffKeepsPaletteExactOutput)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 20, 40, 60, 255 };

	std::array<uint8_t, 1> layerMap { static_cast<uint8_t>(RenderLayer::Cursor) };
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 1, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::Off,
	    { layerMap.data(), 1, 1, 1 },
	});

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 20);
	EXPECT_EQ(color.g, 40);
	EXPECT_EQ(color.b, 60);
}

TEST(FrameCompositor, RenderLayerDiagnosticTintBlendsLayerColor)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 100, 100, 100, 255 };

	std::array<uint8_t, 1> layerMap { static_cast<uint8_t>(RenderLayer::Interface) };
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 1, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::Tint,
	    { layerMap.data(), 1, 1, 1 },
	});

	const SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 50);
	EXPECT_EQ(color.g, 98);
	EXPECT_EQ(color.b, 177);
}

TEST(FrameCompositor, RenderLayerDiagnosticOutlineMarksLayerBoundaries)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 100, 100, 100, 255 };

	std::array<uint8_t, 2> layerMap {
		static_cast<uint8_t>(RenderLayer::World),
		static_cast<uint8_t>(RenderLayer::Interface),
	};
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 2, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::Outline,
	    { layerMap.data(), 2, 1, 2 },
	});

	const SDL_Color worldBoundary = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(worldBoundary.r, 0);
	EXPECT_EQ(worldBoundary.g, 255);
	EXPECT_EQ(worldBoundary.b, 0);

	const SDL_Color interfaceBoundary = ReadColor(*outputSurface, 1, 0);
	EXPECT_EQ(interfaceBoundary.r, 0);
	EXPECT_EQ(interfaceBoundary.g, 96);
	EXPECT_EQ(interfaceBoundary.b, 255);
}

TEST(FrameCompositor, RenderLayerDiagnosticTintAndOutlineCombinesEffects)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;
	pixels[1] = 1;
	pixels[2] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 100, 100, 100, 255 };

	std::array<uint8_t, 3> layerMap {
		static_cast<uint8_t>(RenderLayer::World),
		static_cast<uint8_t>(RenderLayer::Interface),
		static_cast<uint8_t>(RenderLayer::Interface),
	};
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { 3, 1 },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::TintAndOutline,
	    { layerMap.data(), 3, 1, 3 },
	});

	const SDL_Color worldBoundary = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(worldBoundary.r, 0);
	EXPECT_EQ(worldBoundary.g, 255);
	EXPECT_EQ(worldBoundary.b, 0);

	const SDL_Color interfaceBoundary = ReadColor(*outputSurface, 1, 0);
	EXPECT_EQ(interfaceBoundary.r, 0);
	EXPECT_EQ(interfaceBoundary.g, 96);
	EXPECT_EQ(interfaceBoundary.b, 255);

	const SDL_Color interfaceTint = ReadColor(*outputSurface, 2, 0);
	EXPECT_EQ(interfaceTint.r, 50);
	EXPECT_EQ(interfaceTint.g, 98);
	EXPECT_EQ(interfaceTint.b, 177);
}

TEST(FrameCompositor, RenderLayerDiagnosticRecomposesFullFrameWithoutDirtyRects)
{
	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	pixels[0] = 1;

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 100, 100, 100, 255 };

	std::array<uint8_t, 1> layerMap { static_cast<uint8_t>(RenderLayer::World) };
	CompositionFrame frame {
		{ 1, 1 },
		MakeIndexBufferView(*indexSurface),
		MakePaletteSnapshot(palette, 1),
		{},
		false,
		RenderLayerDiagnosticMode::Tint,
		{ layerMap.data(), 1, 1, 1 },
	};
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose(frame);

	SDL_Color color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 50);
	EXPECT_EQ(color.g, 177);
	EXPECT_EQ(color.b, 50);

	layerMap[0] = static_cast<uint8_t>(RenderLayer::Interface);
	compositor.Compose(frame);

	color = ReadColor(*outputSurface, 0, 0);
	EXPECT_EQ(color.r, 50);
	EXPECT_EQ(color.g, 98);
	EXPECT_EQ(color.b, 177);
}

TEST(FrameCompositor, CpuPaletteCompositorComposesLargeDiagnosticFrameAcrossRowBands)
{
	constexpr int Width = 512;
	constexpr int Height = 256;
	const FrameCompositorThreadCountOverrideGuard threadCountOverride { 4 };

	SDLSurfaceUniquePtr indexSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, Width, Height, 8, SDL_PIXELFORMAT_INDEX8);
	auto *pixels = static_cast<uint8_t *>(indexSurface->pixels);
	for (int y = 0; y < Height; y++) {
		for (int x = 0; x < Width; x++) {
			pixels[y * indexSurface->pitch + x] = static_cast<uint8_t>((x + y) % 3 + 1);
		}
	}

	std::array<SDL_Color, 256> palette {};
	palette[1] = { 40, 60, 80, 255 };
	palette[2] = { 80, 100, 120, 255 };
	palette[3] = { 120, 140, 160, 255 };

	std::vector<uint8_t> layerMap(Width * Height, static_cast<uint8_t>(RenderLayer::World));
	SDLSurfaceUniquePtr outputSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, Width, Height, 32, SDL_PIXELFORMAT_RGBA8888);

	CpuPaletteCompositor compositor;
	compositor.SetOutputSurface(outputSurface.get());
	compositor.Compose({
	    { Width, Height },
	    MakeIndexBufferView(*indexSurface),
	    MakePaletteSnapshot(palette, 1),
	    { {}, true },
	    false,
	    RenderLayerDiagnosticMode::TintAndOutline,
	    { layerMap.data(), Width, Height, Width },
	});

	ExpectWorldTint(*outputSurface, 0, 0, palette[1]);
	ExpectWorldTint(*outputSurface, Width - 1, 63, palette[2]);
	ExpectWorldTint(*outputSurface, 17, 64, palette[1]);
	ExpectWorldTint(*outputSurface, Width - 2, 127, palette[2]);
	ExpectWorldTint(*outputSurface, 31, 128, palette[1]);
	ExpectWorldTint(*outputSurface, Width - 3, 191, palette[2]);
	ExpectWorldTint(*outputSurface, 43, 192, palette[2]);
	ExpectWorldTint(*outputSurface, Width - 3, Height - 1, palette[3]);

	const RenderPerfCompositionStats &stats = compositor.GetLastCompositionStats();
	EXPECT_EQ(stats.selectedThreadCount, 4);
}

} // namespace
} // namespace devilution
