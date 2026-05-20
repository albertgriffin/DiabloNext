/**
 * @file render_perf.cpp
 *
 * Lightweight renderer performance telemetry.
 */
#include "engine/render/render_perf.hpp"

#include <algorithm>
#include <cstdint>

#ifdef USE_SDL3
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "utils/log.hpp"

namespace devilution {
namespace {

constexpr uint64_t RenderPerfReportIntervalUs = 1000000;

bool Active = false;
uint64_t NextFrameNumber = 0;
RenderPerfFrameStats CurrentFrame;
RenderPerfRollingStats RollingStats;
bool RollingWindowStarted = false;
uint64_t RollingWindowStartUs = 0;

#ifdef BUILD_TESTING
bool NowUsOverrideEnabled = false;
uint64_t NowUsOverride = 0;
#endif

[[nodiscard]] uint64_t NowUs()
{
#ifdef BUILD_TESTING
	if (NowUsOverrideEnabled)
		return NowUsOverride;
#endif
#ifdef USE_SDL1
	return static_cast<uint64_t>(SDL_GetTicks()) * 1000;
#else
	const uint64_t counter = SDL_GetPerformanceCounter();
	const uint64_t frequency = SDL_GetPerformanceFrequency();
	if (frequency == 0)
		return 0;
	return counter * 1000000 / frequency;
#endif
}

[[nodiscard]] size_t PhaseIndex(const RenderPerfPhase phase)
{
	const size_t index = static_cast<size_t>(phase);
	return index < RenderPerfPhaseCount ? index : 0;
}

[[nodiscard]] uint64_t Average(const uint64_t total, const uint64_t count)
{
	return count == 0 ? 0 : total / count;
}

void ResetRollingStats(const uint64_t nowUs)
{
	RollingStats = {};
	RollingWindowStarted = true;
	RollingWindowStartUs = nowUs;
}

void AccumulateFrame()
{
	RollingStats.frameCount++;
	for (size_t i = 0; i < RenderPerfPhaseCount; i++) {
		RollingStats.phaseUs[i] += CurrentFrame.phaseUs[i];
	}

	const RenderPerfCompositionStats &composition = CurrentFrame.composition;
	if (composition.fullFrameComposed) {
		RollingStats.fullFrameCompositionCount++;
		RollingStats.lastFullFrameReason = composition.fullFrameReason;
	}
	if (composition.parallelCompositionUsed)
		RollingStats.parallelCompositionCount++;
	RollingStats.submittedDirtyRectCount += static_cast<uint64_t>(std::max(composition.submittedDirtyRectCount, 0));
	RollingStats.normalizedDirtyRectCount += static_cast<uint64_t>(std::max(composition.normalizedDirtyRectCount, 0));
	RollingStats.composedRectCount += static_cast<uint64_t>(std::max(composition.composedRectCount, 0));
	RollingStats.submittedDirtyArea += composition.submittedDirtyArea;
	RollingStats.normalizedDirtyArea += composition.normalizedDirtyArea;
	RollingStats.composedPixelArea += composition.composedPixelArea;
	RollingStats.maxSelectedThreadCount = std::max(RollingStats.maxSelectedThreadCount, composition.selectedThreadCount);
	RollingStats.layerStampedSpanCount += CurrentFrame.layerStampedSpanCount;
	RollingStats.layerStampedPixelCount += CurrentFrame.layerStampedPixelCount;
}

void LogRollingStats()
{
	const uint64_t frames = RollingStats.frameCount;
	if (frames == 0)
		return;

	Log("RenderPerf frames={} avg_us cursor_undraw={} world={} interface={} cursor={} debug={} dirty_blit={} compose={} present={} world_detail setup={} lightmap={} floor={} tiles={} oob={} zoom={} overlay={} debug={} view_ui={} layer_setup={} tile_detail cell={} missile={} corpse={} object={} item={} player={} monster={} special={} dirty_rects submitted={} normalized={} composed={} dirty_area submitted={} normalized={} composed={} full_frames={} last_full_reason={} parallel_frames={} max_threads={} layer_spans={} layer_pixels={}",
	    frames,
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::CursorUndraw)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldDraw)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::InterfaceDraw)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::CursorDraw)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::DebugDraw)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::DirtyBlit)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::Compose)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::Present)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldSetup)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldLightmap)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldFloor)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldTiles)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldOob)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldZoom)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldOverlay)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldDebugOverlay)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::ViewInterfaceDraw)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::LayerCaptureSetup)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldTileCell)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldTileMissile)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldTileCorpse)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldTileObject)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldTileItem)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldTilePlayer)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldTileMonster)], frames),
	    Average(RollingStats.phaseUs[PhaseIndex(RenderPerfPhase::WorldTileSpecial)], frames),
	    Average(RollingStats.submittedDirtyRectCount, frames),
	    Average(RollingStats.normalizedDirtyRectCount, frames),
	    Average(RollingStats.composedRectCount, frames),
	    Average(RollingStats.submittedDirtyArea, frames),
	    Average(RollingStats.normalizedDirtyArea, frames),
	    Average(RollingStats.composedPixelArea, frames),
	    RollingStats.fullFrameCompositionCount,
	    CompositionFullFrameReasonName(RollingStats.lastFullFrameReason),
	    RollingStats.parallelCompositionCount,
	    RollingStats.maxSelectedThreadCount,
	    Average(RollingStats.layerStampedSpanCount, frames),
	    Average(RollingStats.layerStampedPixelCount, frames));
}

} // namespace

std::string_view CompositionFullFrameReasonName(const CompositionFullFrameReason reason)
{
	switch (reason) {
	case CompositionFullFrameReason::None:
		return "none";
	case CompositionFullFrameReason::Requested:
		return "requested";
	case CompositionFullFrameReason::FirstFrame:
		return "first-frame";
	case CompositionFullFrameReason::PaletteChanged:
		return "palette-changed";
	case CompositionFullFrameReason::DiagnosticTransformChanged:
		return "diagnostic-transform-changed";
	case CompositionFullFrameReason::RenderLayerDiagnosticModeChanged:
		return "render-layer-diagnostic-mode-changed";
	case CompositionFullFrameReason::RenderLayerDiagnosticsRequested:
		return "render-layer-diagnostics-requested";
	case CompositionFullFrameReason::TooManyDirtyRects:
		return "too-many-dirty-rects";
	case CompositionFullFrameReason::DirtyAreaTooLarge:
		return "dirty-area-too-large";
	case CompositionFullFrameReason::OutputSurfaceChanged:
		return "output-surface-changed";
	case CompositionFullFrameReason::IndexBufferChanged:
		return "index-buffer-changed";
	case CompositionFullFrameReason::LogicalSizeChanged:
		return "logical-size-changed";
	}
	return "unknown";
}

bool RenderPerfActive()
{
	return Active;
}

const RenderPerfFrameStats &GetRenderPerfFrameStats()
{
	return CurrentFrame;
}

const RenderPerfRollingStats &GetRenderPerfRollingStats()
{
	return RollingStats;
}

void BeginRenderPerfFrame(const bool enabled)
{
	Active = enabled;
	CurrentFrame = {};
	if (!Active)
		return;

	CurrentFrame.frameNumber = ++NextFrameNumber;
}

void EndRenderPerfFrame()
{
	if (!Active)
		return;

	const uint64_t nowUs = NowUs();
	if (!RollingWindowStarted)
		ResetRollingStats(nowUs);

	AccumulateFrame();
	if (nowUs - RollingWindowStartUs >= RenderPerfReportIntervalUs) {
		LogRollingStats();
		ResetRollingStats(nowUs);
	}

	Active = false;
}

void AddRenderPerfDuration(const RenderPerfPhase phase, const uint64_t durationUs)
{
	if (!Active)
		return;

	CurrentFrame.phaseUs[PhaseIndex(phase)] += durationUs;
}

void SetRenderPerfCompositionStats(const RenderPerfCompositionStats &stats)
{
	if (!Active)
		return;

	CurrentFrame.composition = stats;
}

void SetRenderPerfLayerCaptureStats(const uint64_t stampedSpanCount, const uint64_t stampedPixelCount)
{
	if (!Active)
		return;

	CurrentFrame.layerStampedSpanCount = stampedSpanCount;
	CurrentFrame.layerStampedPixelCount = stampedPixelCount;
}

RenderPerfScope::RenderPerfScope(const RenderPerfPhase phase)
    : RenderPerfScope(phase, RenderPerfActive())
{
}

RenderPerfScope::RenderPerfScope(const RenderPerfPhase phase, const bool active)
    : phase_(phase)
    , active_(active)
{
	if (active_)
		startUs_ = NowUs();
}

RenderPerfScope::~RenderPerfScope()
{
	if (!active_)
		return;

	const uint64_t endUs = NowUs();
	AddRenderPerfDuration(phase_, endUs > startUs_ ? endUs - startUs_ : 0);
}

#ifdef BUILD_TESTING
void ResetRenderPerfStatsForTesting()
{
	Active = false;
	NextFrameNumber = 0;
	CurrentFrame = {};
	RollingStats = {};
	RollingWindowStarted = false;
	RollingWindowStartUs = 0;
	NowUsOverrideEnabled = false;
	NowUsOverride = 0;
}

void SetRenderPerfNowUsForTesting(const uint64_t nowUs)
{
	NowUsOverrideEnabled = true;
	NowUsOverride = nowUs;
}

void ClearRenderPerfNowUsForTesting()
{
	NowUsOverrideEnabled = false;
	NowUsOverride = 0;
}
#endif

} // namespace devilution
