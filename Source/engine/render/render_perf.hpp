/**
 * @file render_perf.hpp
 *
 * Lightweight renderer performance telemetry.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#ifdef BUILD_TESTING
#include "utils/attributes.h"
#endif

namespace devilution {

enum class RenderPerfPhase : uint8_t {
	CursorUndraw,
	WorldDraw,
	InterfaceDraw,
	CursorDraw,
	DebugDraw,
	DirtyBlit,
	Compose,
	Present,
	WorldSetup,
	WorldLightmap,
	WorldFloor,
	WorldTiles,
	WorldOob,
	WorldZoom,
	WorldOverlay,
	WorldDebugOverlay,
	ViewInterfaceDraw,
	LayerCaptureSetup,
	WorldTileCell,
	WorldTileMissile,
	WorldTileCorpse,
	WorldTileObject,
	WorldTileItem,
	WorldTilePlayer,
	WorldTileMonster,
	WorldTileSpecial,
	Count,
};

inline constexpr size_t RenderPerfPhaseCount = static_cast<size_t>(RenderPerfPhase::Count);

enum class CompositionFullFrameReason : uint8_t {
	None,
	Requested,
	FirstFrame,
	PaletteChanged,
	DiagnosticTransformChanged,
	RenderLayerDiagnosticModeChanged,
	RenderLayerDiagnosticsRequested,
	TooManyDirtyRects,
	DirtyAreaTooLarge,
	OutputSurfaceChanged,
	IndexBufferChanged,
	LogicalSizeChanged,
	DirectPresentationUnavailable,
};

struct RenderPerfCompositionStats {
	bool compositorEnabled = false;
	bool layerCaptureEnabled = false;
	bool fullFrameComposed = false;
	CompositionFullFrameReason fullFrameReason = CompositionFullFrameReason::None;
	int submittedDirtyRectCount = 0;
	int normalizedDirtyRectCount = 0;
	int composedRectCount = 0;
	uint64_t submittedDirtyArea = 0;
	uint64_t normalizedDirtyArea = 0;
	uint64_t composedPixelArea = 0;
	int selectedThreadCount = 0;
	bool parallelCompositionUsed = false;
};

struct RenderPerfFrameStats {
	uint64_t frameNumber = 0;
	std::array<uint64_t, RenderPerfPhaseCount> phaseUs {};
	RenderPerfCompositionStats composition;
	uint64_t layerStampedSpanCount = 0;
	uint64_t layerStampedPixelCount = 0;
};

struct RenderPerfRollingStats {
	uint64_t frameCount = 0;
	std::array<uint64_t, RenderPerfPhaseCount> phaseUs {};
	uint64_t fullFrameCompositionCount = 0;
	uint64_t parallelCompositionCount = 0;
	uint64_t submittedDirtyRectCount = 0;
	uint64_t normalizedDirtyRectCount = 0;
	uint64_t composedRectCount = 0;
	uint64_t submittedDirtyArea = 0;
	uint64_t normalizedDirtyArea = 0;
	uint64_t composedPixelArea = 0;
	uint64_t layerStampedSpanCount = 0;
	uint64_t layerStampedPixelCount = 0;
	int maxSelectedThreadCount = 0;
	CompositionFullFrameReason lastFullFrameReason = CompositionFullFrameReason::None;
};

[[nodiscard]] std::string_view CompositionFullFrameReasonName(CompositionFullFrameReason reason);
[[nodiscard]] bool RenderPerfActive();
[[nodiscard]] const RenderPerfFrameStats &GetRenderPerfFrameStats();
[[nodiscard]] const RenderPerfRollingStats &GetRenderPerfRollingStats();

void BeginRenderPerfFrame(bool enabled);
void EndRenderPerfFrame();
void AddRenderPerfDuration(RenderPerfPhase phase, uint64_t durationUs);
void SetRenderPerfCompositionStats(const RenderPerfCompositionStats &stats);
void SetRenderPerfLayerCaptureStats(uint64_t stampedSpanCount, uint64_t stampedPixelCount);

class RenderPerfScope {
public:
	explicit RenderPerfScope(RenderPerfPhase phase);
	RenderPerfScope(RenderPerfPhase phase, bool active);
	~RenderPerfScope();

	RenderPerfScope(const RenderPerfScope &) = delete;
	RenderPerfScope &operator=(const RenderPerfScope &) = delete;

private:
	RenderPerfPhase phase_;
	uint64_t startUs_ = 0;
	bool active_ = false;
};

#ifdef BUILD_TESTING
void DVL_API_FOR_TEST ResetRenderPerfStatsForTesting();
void DVL_API_FOR_TEST SetRenderPerfNowUsForTesting(uint64_t nowUs);
void DVL_API_FOR_TEST ClearRenderPerfNowUsForTesting();
#endif

} // namespace devilution
