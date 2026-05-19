/**
 * @file sdl_gpu_palette_compositor.hpp
 *
 * Optional SDL_GPU palette compositor target.
 */
#pragma once

#include <memory>

#include "engine/render/accelerated_compositor_lifecycle.hpp"

namespace devilution {

class IFrameCompositorBackend;

[[nodiscard]] bool SdlGpuPaletteCompositorRequested();
[[nodiscard]] bool SdlGpuPaletteCompositorWindowRequested();
[[nodiscard]] AcceleratedCompositorWindowFlags ConfigureSdlGpuPaletteCompositorWindow();
[[nodiscard]] bool ReinitializeSdlGpuPaletteCompositor(SDL_Window *window);
[[nodiscard]] bool SdlGpuPaletteCompositorIsActive();
void ShutdownSdlGpuPaletteCompositor();
[[nodiscard]] std::unique_ptr<IFrameCompositorBackend> CreateSdlGpuPaletteCompositorBackend();

} // namespace devilution
