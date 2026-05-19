#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "engine/render/clx_render.hpp"
#include "engine/render/primitive_render.hpp"
#include "engine/render/render_layer.hpp"
#include "engine/surface.hpp"
#include "utils/sdl_geometry.h"
#include "utils/sdl_wrap.h"

namespace devilution {
namespace {

uint8_t LayerAt(const int x, const int y)
{
	const RenderLayerMapView layerMap = CurrentRenderLayerMapView();
	return layerMap.pixels[static_cast<size_t>(y) * layerMap.pitch + x];
}

void BeginTestRenderLayerFrame(const Surface &out)
{
	ResetRenderLayerFrameStats();
	BeginRenderLayerFrame(out, true);
}

TEST(RenderLayer, ScopeRestoresPreviousLayer)
{
	ResetRenderLayerFrameStats();

	EXPECT_EQ(CurrentRenderLayer(), RenderLayer::World);
	{
		RenderLayerScope interfaceLayer(RenderLayer::Interface);
		EXPECT_EQ(CurrentRenderLayer(), RenderLayer::Interface);
		{
			RenderLayerScope cursorLayer(RenderLayer::Cursor);
			EXPECT_EQ(CurrentRenderLayer(), RenderLayer::Cursor);
		}
		EXPECT_EQ(CurrentRenderLayer(), RenderLayer::Interface);
	}
	EXPECT_EQ(CurrentRenderLayer(), RenderLayer::World);

	const RenderLayerFrameStats &stats = GetRenderLayerFrameStats();
	EXPECT_EQ(stats.layers[static_cast<size_t>(RenderLayer::Interface)].enterCount, 1);
	EXPECT_EQ(stats.layers[static_cast<size_t>(RenderLayer::Cursor)].enterCount, 1);
}

TEST(RenderLayer, TracksDirtyRectsByLayer)
{
	ResetRenderLayerFrameStats();

	RecordRenderLayerDirtyRect(CurrentRenderLayer());
	{
		RenderLayerScope overlayLayer(RenderLayer::WorldOverlay);
		RecordRenderLayerDirtyRect(CurrentRenderLayer());
		RecordRenderLayerDirtyRect(CurrentRenderLayer());
	}

	const RenderLayerFrameStats &stats = GetRenderLayerFrameStats();
	EXPECT_EQ(stats.layers[static_cast<size_t>(RenderLayer::World)].dirtyRectCount, 1);
	EXPECT_EQ(stats.layers[static_cast<size_t>(RenderLayer::WorldOverlay)].dirtyRectCount, 2);
}

TEST(RenderLayer, CapturesLayerOwnershipForChangedPixels)
{
	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface out(surface.get());

	BeginTestRenderLayerFrame(out);
	{
		RenderLayerScope overlayLayer(RenderLayer::WorldOverlay);
		out.SetPixel({ 0, 0 }, 1);
		{
			RenderLayerScope interfaceLayer(RenderLayer::Interface);
			out.SetPixel({ 1, 0 }, 2);
		}
		out.SetPixel({ 2, 0 }, 3);
	}

	EXPECT_EQ(LayerAt(0, 0), static_cast<uint8_t>(RenderLayer::WorldOverlay));
	EXPECT_EQ(LayerAt(1, 0), static_cast<uint8_t>(RenderLayer::Interface));
	EXPECT_EQ(LayerAt(2, 0), static_cast<uint8_t>(RenderLayer::WorldOverlay));
}

TEST(RenderLayer, ParentScopeCanOverwriteChildOwnedPixelsAfterChildExits)
{
	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface out(surface.get());

	BeginTestRenderLayerFrame(out);
	{
		RenderLayerScope overlayLayer(RenderLayer::WorldOverlay);
		out.SetPixel({ 0, 0 }, 1);
		{
			RenderLayerScope interfaceLayer(RenderLayer::Interface);
			out.SetPixel({ 0, 0 }, 2);
		}
		out.SetPixel({ 0, 0 }, 3);
	}

	EXPECT_EQ(LayerAt(0, 0), static_cast<uint8_t>(RenderLayer::WorldOverlay));
}

TEST(RenderLayer, StampsWritesEvenWhenPixelValueDoesNotChange)
{
	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface out(surface.get());

	BeginTestRenderLayerFrame(out);
	{
		RenderLayerScope interfaceLayer(RenderLayer::Interface);
		out.SetPixel({ 0, 0 }, 0);
	}

	EXPECT_EQ(LayerAt(0, 0), static_cast<uint8_t>(RenderLayer::Interface));
}

TEST(RenderLayer, SurfaceBlitsStampCopiedPixels)
{
	SDLSurfaceUniquePtr srcSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface src(srcSurface.get());
	src.SetPixel({ 0, 0 }, 4);
	src.SetPixel({ 1, 0 }, 5);

	SDLSurfaceUniquePtr dstSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface out(dstSurface.get());

	BeginTestRenderLayerFrame(out);
	{
		RenderLayerScope interfaceLayer(RenderLayer::Interface);
		out.BlitFrom(src, MakeSdlRect(0, 0, 2, 1), { 1, 0 });
	}

	EXPECT_EQ(LayerAt(0, 0), UnknownRenderLayerId);
	EXPECT_EQ(LayerAt(1, 0), static_cast<uint8_t>(RenderLayer::Interface));
	EXPECT_EQ(LayerAt(2, 0), static_cast<uint8_t>(RenderLayer::Interface));
}

TEST(RenderLayer, SkipZeroBlitsStampOnlyCopiedPixels)
{
	SDLSurfaceUniquePtr srcSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface src(srcSurface.get());
	src.SetPixel({ 0, 0 }, 6);
	src.SetPixel({ 1, 0 }, 0);
	src.SetPixel({ 2, 0 }, 7);

	SDLSurfaceUniquePtr dstSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface out(dstSurface.get());

	BeginTestRenderLayerFrame(out);
	{
		RenderLayerScope cursorLayer(RenderLayer::Cursor);
		out.BlitFromSkipColorIndexZero(src, MakeSdlRect(0, 0, 3, 1), { 0, 0 });
	}

	EXPECT_EQ(LayerAt(0, 0), static_cast<uint8_t>(RenderLayer::Cursor));
	EXPECT_EQ(LayerAt(1, 0), UnknownRenderLayerId);
	EXPECT_EQ(LayerAt(2, 0), static_cast<uint8_t>(RenderLayer::Cursor));
}

TEST(RenderLayer, PrimitiveSpansStampLayer)
{
	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface out(surface.get());

	BeginTestRenderLayerFrame(out);
	{
		RenderLayerScope debugLayer(RenderLayer::Debug);
		DrawHorizontalLine(out, { 1, 0 }, 2, 8);
	}

	EXPECT_EQ(LayerAt(0, 0), UnknownRenderLayerId);
	EXPECT_EQ(LayerAt(1, 0), static_cast<uint8_t>(RenderLayer::Debug));
	EXPECT_EQ(LayerAt(2, 0), static_cast<uint8_t>(RenderLayer::Debug));
}

TEST(RenderLayer, TracksLayerCaptureStampCost)
{
	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 4, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface out(surface.get());

	BeginTestRenderLayerFrame(out);
	{
		RenderLayerScope debugLayer(RenderLayer::Debug);
		out.SetPixel({ 0, 0 }, 1);
		DrawHorizontalLine(out, { 1, 0 }, 3, 8);
	}

	const RenderLayerFrameStats &stats = GetRenderLayerFrameStats();
	EXPECT_EQ(stats.stampedSpanCount, 2);
	EXPECT_EQ(stats.stampedPixelCount, 4);
}

TEST(RenderLayer, ClxOpaqueRunsStampLayer)
{
	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 3, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface out(surface.get());
	const std::array<uint8_t, 9> spriteData { 6, 0, 2, 0, 1, 0, 0xFE, 9, 10 };
	const ClxSprite sprite(spriteData.data(), static_cast<uint32_t>(spriteData.size()));

	BeginTestRenderLayerFrame(out);
	{
		RenderLayerScope overlayLayer(RenderLayer::WorldOverlay);
		ClxDraw(out, { 1, 0 }, sprite);
	}

	EXPECT_EQ(LayerAt(0, 0), UnknownRenderLayerId);
	EXPECT_EQ(LayerAt(1, 0), static_cast<uint8_t>(RenderLayer::WorldOverlay));
	EXPECT_EQ(LayerAt(2, 0), static_cast<uint8_t>(RenderLayer::WorldOverlay));
}

TEST(RenderLayer, SuspendedCapturePreservesExistingLayerOwnership)
{
	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface out(surface.get());

	BeginTestRenderLayerFrame(out);
	{
		RenderLayerScope worldLayer(RenderLayer::World);
		out.SetPixel({ 0, 0 }, 1);
	}
	{
		RenderLayerCaptureSuspension captureSuspension;
		out.SetPixel({ 0, 0 }, 2);
	}
	{
		RenderLayerScope cursorLayer(RenderLayer::Cursor, { { 1, 0 }, { 1, 1 } });
		out.SetPixel({ 1, 0 }, 3);
	}

	EXPECT_EQ(LayerAt(0, 0), static_cast<uint8_t>(RenderLayer::World));
	EXPECT_EQ(LayerAt(1, 0), static_cast<uint8_t>(RenderLayer::Cursor));
}

TEST(RenderLayer, RestoredCursorPixelsRestoreSavedLayerOwnership)
{
	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface out(surface.get());
	const Rectangle cursorRect { { 0, 0 }, { 1, 1 } };
	uint8_t savedLayers[1] {};

	BeginTestRenderLayerFrame(out);
	{
		RenderLayerScope worldLayer(RenderLayer::World);
		out.SetPixel({ 0, 0 }, 1);
	}
	EXPECT_TRUE(SaveRenderLayerMapRegion(cursorRect, savedLayers, 1));
	{
		RenderLayerScope cursorLayer(RenderLayer::Cursor);
		out.SetPixel({ 0, 0 }, 2);
	}
	EXPECT_EQ(LayerAt(0, 0), static_cast<uint8_t>(RenderLayer::Cursor));

	out.SetPixel({ 0, 0 }, 1);
	EXPECT_TRUE(RestoreRenderLayerMapRegion(cursorRect, savedLayers, 1));
	EXPECT_EQ(LayerAt(0, 0), static_cast<uint8_t>(RenderLayer::World));
}

TEST(RenderLayer, LayerMapPersistsForSameBufferAndResetsForNewBuffer)
{
	SDLSurfaceUniquePtr firstSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface firstOut(firstSurface.get());

	BeginTestRenderLayerFrame(firstOut);
	{
		RenderLayerScope worldLayer(RenderLayer::World);
		firstOut.SetPixel({ 0, 0 }, 1);
	}
	EXPECT_EQ(LayerAt(0, 0), static_cast<uint8_t>(RenderLayer::World));

	BeginRenderLayerFrame(firstOut, true);
	EXPECT_EQ(LayerAt(0, 0), static_cast<uint8_t>(RenderLayer::World));

	BeginRenderLayerFrame(firstOut, false);
	EXPECT_EQ(CurrentRenderLayerMapView().pixels, nullptr);
	BeginRenderLayerFrame(firstOut, true);
	EXPECT_EQ(LayerAt(0, 0), UnknownRenderLayerId);

	SDLSurfaceUniquePtr secondSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 1, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface secondOut(secondSurface.get());
	BeginRenderLayerFrame(secondOut, true);
	EXPECT_EQ(LayerAt(0, 0), UnknownRenderLayerId);

	SDLSurfaceUniquePtr resizedSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2, 1, 8, SDL_PIXELFORMAT_INDEX8);
	Surface resizedOut(resizedSurface.get());
	BeginRenderLayerFrame(resizedOut, true);
	EXPECT_EQ(LayerAt(0, 0), UnknownRenderLayerId);
	EXPECT_EQ(LayerAt(1, 0), UnknownRenderLayerId);
}

} // namespace
} // namespace devilution
