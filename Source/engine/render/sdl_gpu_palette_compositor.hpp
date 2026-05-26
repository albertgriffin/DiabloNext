/**
 * @file sdl_gpu_palette_compositor.hpp
 *
 * Optional SDL_GPU palette compositor target.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "engine/displacement.hpp"
#include "engine/point.hpp"
#include "engine/rectangle.hpp"
#include "engine/render/accelerated_compositor_lifecycle.hpp"
#include "engine/render/automap_render.hpp"
#include "engine/size.hpp"

namespace devilution {

class IFrameCompositorBackend;

struct SdlGpuPaletteCursorOverlay {
	const uint8_t *rgbaPixels = nullptr;
	Size size {};
	int pitch = 0;
	Point position {};
	uint64_t version = 0;
	bool visible = false;
};

[[nodiscard]] bool SdlGpuPaletteCompositorBuildAvailable();
[[nodiscard]] bool SdlGpuPaletteCompositorRequested();
[[nodiscard]] bool SdlGpuPaletteCompositorWindowRequested();
[[nodiscard]] AcceleratedCompositorWindowFlags ConfigureSdlGpuPaletteCompositorWindow();
[[nodiscard]] bool ReinitializeSdlGpuPaletteCompositor(SDL_Window *window);
[[nodiscard]] bool SdlGpuPaletteCompositorIsActive();
[[nodiscard]] bool SdlGpuPaletteCompositorGpuCursorOverlayAvailable();
[[nodiscard]] bool SdlGpuPaletteCompositorGpuAutomapOverlayAvailable();
void SetSdlGpuPaletteCompositorCursorOverlay(const SdlGpuPaletteCursorOverlay &overlay);
void ClearSdlGpuPaletteCompositorCursorOverlay();
void SetSdlGpuPaletteCompositorAutomapOverlay(RenderAutomapOverlayView overlay);
void SetSdlGpuPaletteCompositorAutomapOverlayOffset(Displacement offset);
void SetSdlGpuPaletteCompositorAutomapOverlayRejectRects(const Rectangle *rects, std::size_t count);
void SetSdlGpuPaletteCompositorAutomapPlayerOverlay(RenderAutomapOverlayView overlay);
void ClearSdlGpuPaletteCompositorAutomapOverlay();
void ShutdownSdlGpuPaletteCompositor();
[[nodiscard]] std::unique_ptr<IFrameCompositorBackend> CreateSdlGpuPaletteCompositorBackend();

} // namespace devilution
