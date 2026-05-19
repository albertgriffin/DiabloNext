/**
 * @file accelerated_compositor_lifecycle.hpp
 *
 * Lifecycle routing for optional accelerated frame compositor backends.
 */
#pragma once

#include <cstdint>
#include <memory>

#ifdef USE_SDL3
#include <SDL3/SDL_video.h>
#else
#include <SDL.h>
#endif

namespace devilution {

class IFrameCompositorBackend;
enum class RenderFrameCompositorBackend : uint8_t;

[[nodiscard]] bool AcceleratedFrameCompositorRequested();
[[nodiscard]] bool AcceleratedFrameCompositorWindowRequested();
void ConfigureAcceleratedFrameCompositorWindow();
[[nodiscard]] bool ReinitializeAcceleratedFrameCompositor(SDL_Window *window);
[[nodiscard]] bool AcceleratedFrameCompositorIsActive();
void ShutdownAcceleratedFrameCompositor();
[[nodiscard]] std::unique_ptr<IFrameCompositorBackend> CreateAcceleratedFrameCompositorBackend(RenderFrameCompositorBackend backend);

} // namespace devilution
