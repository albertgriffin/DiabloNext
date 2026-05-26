/**
 * @file opengl_palette_compositor.hpp
 *
 * Optional OpenGL palette compositor spike.
 */
#pragma once

#include <memory>

#include "engine/render/accelerated_compositor_lifecycle.hpp"

#ifdef USE_SDL3
#include <SDL3/SDL_video.h>
#else
#include <SDL.h>
#endif

namespace devilution {

class IFrameCompositorBackend;

[[nodiscard]] bool OpenGlPaletteCompositorBuildAvailable();
[[nodiscard]] bool OpenGlPaletteCompositorRequested();
[[nodiscard]] bool OpenGlPaletteCompositorWindowRequested();
[[nodiscard]] AcceleratedCompositorWindowFlags ConfigureOpenGlPaletteCompositorWindow();
[[nodiscard]] bool ReinitializeOpenGlPaletteCompositor(SDL_Window *window);
[[nodiscard]] bool OpenGlPaletteCompositorIsActive();
void ShutdownOpenGlPaletteCompositor();
[[nodiscard]] std::unique_ptr<IFrameCompositorBackend> CreateOpenGlPaletteCompositorBackend();

} // namespace devilution
