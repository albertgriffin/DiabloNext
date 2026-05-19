/**
 * @file opengl_palette_compositor.hpp
 *
 * Optional OpenGL palette compositor spike.
 */
#pragma once

#include <memory>

#ifdef USE_SDL3
#include <SDL3/SDL_video.h>
#else
#include <SDL.h>
#endif

namespace devilution {

class IFrameCompositorBackend;

[[nodiscard]] bool OpenGlPaletteCompositorRequested();
[[nodiscard]] bool OpenGlPaletteCompositorWindowRequested();
void ConfigureOpenGlPaletteCompositorWindow();
[[nodiscard]] bool ReinitializeOpenGlPaletteCompositor(SDL_Window *window);
[[nodiscard]] bool OpenGlPaletteCompositorIsActive();
void ShutdownOpenGlPaletteCompositor();
[[nodiscard]] std::unique_ptr<IFrameCompositorBackend> CreateOpenGlPaletteCompositorBackend();

} // namespace devilution
