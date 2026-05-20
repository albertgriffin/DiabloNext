#include <gtest/gtest.h>

#include "engine/render/render_perf.hpp"

namespace devilution {
namespace {

class RenderPerfTest : public testing::Test {
protected:
	void SetUp() override
	{
		ResetRenderPerfStatsForTesting();
	}

	void TearDown() override
	{
		ResetRenderPerfStatsForTesting();
	}
};

TEST_F(RenderPerfTest, AccumulatesFrameDurationsIntoRollingStats)
{
	SetRenderPerfNowUsForTesting(100);
	BeginRenderPerfFrame(true);
	AddRenderPerfDuration(RenderPerfPhase::WorldDraw, 11);
	AddRenderPerfDuration(RenderPerfPhase::WorldDraw, 7);
	AddRenderPerfDuration(RenderPerfPhase::WorldTiles, 13);
	AddRenderPerfDuration(RenderPerfPhase::WorldTileCell, 8);
	AddRenderPerfDuration(RenderPerfPhase::WorldTileMonster, 2);
	AddRenderPerfDuration(RenderPerfPhase::ViewInterfaceDraw, 3);
	AddRenderPerfDuration(RenderPerfPhase::LayerCaptureSetup, 2);
	AddRenderPerfDuration(RenderPerfPhase::DirtyBlit, 5);
	EndRenderPerfFrame();

	const RenderPerfFrameStats &frame = GetRenderPerfFrameStats();
	EXPECT_EQ(frame.frameNumber, 1);
	EXPECT_EQ(frame.phaseUs[static_cast<size_t>(RenderPerfPhase::WorldDraw)], 18);
	EXPECT_EQ(frame.phaseUs[static_cast<size_t>(RenderPerfPhase::WorldTiles)], 13);
	EXPECT_EQ(frame.phaseUs[static_cast<size_t>(RenderPerfPhase::WorldTileCell)], 8);
	EXPECT_EQ(frame.phaseUs[static_cast<size_t>(RenderPerfPhase::WorldTileMonster)], 2);
	EXPECT_EQ(frame.phaseUs[static_cast<size_t>(RenderPerfPhase::ViewInterfaceDraw)], 3);
	EXPECT_EQ(frame.phaseUs[static_cast<size_t>(RenderPerfPhase::LayerCaptureSetup)], 2);
	EXPECT_EQ(frame.phaseUs[static_cast<size_t>(RenderPerfPhase::DirtyBlit)], 5);

	const RenderPerfRollingStats &rolling = GetRenderPerfRollingStats();
	EXPECT_EQ(rolling.frameCount, 1);
	EXPECT_EQ(rolling.phaseUs[static_cast<size_t>(RenderPerfPhase::WorldDraw)], 18);
	EXPECT_EQ(rolling.phaseUs[static_cast<size_t>(RenderPerfPhase::WorldTiles)], 13);
	EXPECT_EQ(rolling.phaseUs[static_cast<size_t>(RenderPerfPhase::WorldTileCell)], 8);
	EXPECT_EQ(rolling.phaseUs[static_cast<size_t>(RenderPerfPhase::WorldTileMonster)], 2);
	EXPECT_EQ(rolling.phaseUs[static_cast<size_t>(RenderPerfPhase::ViewInterfaceDraw)], 3);
	EXPECT_EQ(rolling.phaseUs[static_cast<size_t>(RenderPerfPhase::LayerCaptureSetup)], 2);
	EXPECT_EQ(rolling.phaseUs[static_cast<size_t>(RenderPerfPhase::DirtyBlit)], 5);
}

TEST_F(RenderPerfTest, ScopedTimerUsesInjectedClock)
{
	SetRenderPerfNowUsForTesting(200);
	BeginRenderPerfFrame(true);
	{
		RenderPerfScope scope(RenderPerfPhase::CursorDraw);
		SetRenderPerfNowUsForTesting(275);
	}
	EndRenderPerfFrame();

	EXPECT_EQ(GetRenderPerfFrameStats().phaseUs[static_cast<size_t>(RenderPerfPhase::CursorDraw)], 75);
}

TEST_F(RenderPerfTest, ScopedTimerHonorsExplicitInactiveFlag)
{
	SetRenderPerfNowUsForTesting(200);
	BeginRenderPerfFrame(true);
	{
		RenderPerfScope scope(RenderPerfPhase::WorldTileCell, false);
		SetRenderPerfNowUsForTesting(275);
	}
	EndRenderPerfFrame();

	EXPECT_EQ(GetRenderPerfFrameStats().phaseUs[static_cast<size_t>(RenderPerfPhase::WorldTileCell)], 0);
}

TEST_F(RenderPerfTest, RecordsCompositionAndLayerCaptureStats)
{
	SetRenderPerfNowUsForTesting(300);
	BeginRenderPerfFrame(true);

	RenderPerfCompositionStats composition;
	composition.compositorEnabled = true;
	composition.fullFrameComposed = true;
	composition.fullFrameReason = CompositionFullFrameReason::PaletteChanged;
	composition.submittedDirtyRectCount = 2;
	composition.normalizedDirtyRectCount = 1;
	composition.composedRectCount = 1;
	composition.submittedDirtyArea = 20;
	composition.normalizedDirtyArea = 12;
	composition.composedPixelArea = 12;
	composition.selectedThreadCount = 3;
	composition.parallelCompositionUsed = true;
	composition.worldRoleDirtyRectCount = 1;
	composition.interfaceRoleDirtyRectCount = 2;
	composition.cursorRoleDirtyRectCount = 1;
	composition.worldRoleDirtyPixelArea = 9;
	composition.interfaceRoleDirtyPixelArea = 7;
	composition.cursorRoleDirtyPixelArea = 4;
	SetRenderPerfCompositionStats(composition);
	SetRenderPerfLayerCaptureStats(4, 40);
	SetRenderPerfWorldMaskStats(3, 30);

	EndRenderPerfFrame();

	const RenderPerfFrameStats &frame = GetRenderPerfFrameStats();
	EXPECT_TRUE(frame.composition.compositorEnabled);
	EXPECT_EQ(frame.composition.fullFrameReason, CompositionFullFrameReason::PaletteChanged);
	EXPECT_EQ(frame.layerStampedSpanCount, 4);
	EXPECT_EQ(frame.layerStampedPixelCount, 40);
	EXPECT_EQ(frame.worldMaskStampedSpanCount, 3);
	EXPECT_EQ(frame.worldMaskStampedPixelCount, 30);

	const RenderPerfRollingStats &rolling = GetRenderPerfRollingStats();
	EXPECT_EQ(rolling.fullFrameCompositionCount, 1);
	EXPECT_EQ(rolling.parallelCompositionCount, 1);
	EXPECT_EQ(rolling.submittedDirtyRectCount, 2);
	EXPECT_EQ(rolling.normalizedDirtyRectCount, 1);
	EXPECT_EQ(rolling.composedRectCount, 1);
	EXPECT_EQ(rolling.submittedDirtyArea, 20);
	EXPECT_EQ(rolling.normalizedDirtyArea, 12);
	EXPECT_EQ(rolling.composedPixelArea, 12);
	EXPECT_EQ(rolling.maxSelectedThreadCount, 3);
	EXPECT_EQ(rolling.worldRoleDirtyRectCount, 1);
	EXPECT_EQ(rolling.interfaceRoleDirtyRectCount, 2);
	EXPECT_EQ(rolling.cursorRoleDirtyRectCount, 1);
	EXPECT_EQ(rolling.worldRoleDirtyPixelArea, 9);
	EXPECT_EQ(rolling.interfaceRoleDirtyPixelArea, 7);
	EXPECT_EQ(rolling.cursorRoleDirtyPixelArea, 4);
	EXPECT_EQ(rolling.layerStampedSpanCount, 4);
	EXPECT_EQ(rolling.layerStampedPixelCount, 40);
	EXPECT_EQ(rolling.worldMaskStampedSpanCount, 3);
	EXPECT_EQ(rolling.worldMaskStampedPixelCount, 30);
}

TEST_F(RenderPerfTest, ResetsRollingStatsAfterReportWindow)
{
	SetRenderPerfNowUsForTesting(0);
	BeginRenderPerfFrame(true);
	AddRenderPerfDuration(RenderPerfPhase::WorldDraw, 5);
	EndRenderPerfFrame();
	EXPECT_EQ(GetRenderPerfRollingStats().frameCount, 1);

	SetRenderPerfNowUsForTesting(1000000);
	BeginRenderPerfFrame(true);
	AddRenderPerfDuration(RenderPerfPhase::WorldDraw, 7);
	EndRenderPerfFrame();

	EXPECT_EQ(GetRenderPerfRollingStats().frameCount, 0);
	EXPECT_EQ(GetRenderPerfRollingStats().phaseUs[static_cast<size_t>(RenderPerfPhase::WorldDraw)], 0);
}

} // namespace
} // namespace devilution
