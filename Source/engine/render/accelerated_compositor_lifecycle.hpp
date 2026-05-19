/**
 * @file accelerated_compositor_lifecycle.hpp
 *
 * Lifecycle routing for optional accelerated frame compositor backends.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#ifdef USE_SDL3
#include <SDL3/SDL_video.h>
#else
#include <SDL.h>
#endif

namespace devilution {

class IFrameCompositorBackend;
enum class RenderFrameCompositorBackend : uint8_t;

#ifdef USE_SDL3
using AcceleratedCompositorWindowFlags = SDL_WindowFlags;
#else
using AcceleratedCompositorWindowFlags = int;
#endif

enum class AcceleratedCompositorApi : uint8_t {
	None,
	OpenGl,
	// Production acceleration target. It is not selectable until a backend is added.
	SdlGpu,
};

[[nodiscard]] std::string_view AcceleratedCompositorApiName(AcceleratedCompositorApi api);
[[nodiscard]] AcceleratedCompositorApi AcceleratedFrameCompositorRequestedApi();
[[nodiscard]] AcceleratedCompositorApi AcceleratedFrameCompositorActiveApi();
[[nodiscard]] bool AcceleratedFrameCompositorRequested();
[[nodiscard]] bool AcceleratedFrameCompositorWindowRequested();
[[nodiscard]] AcceleratedCompositorWindowFlags ConfigureAcceleratedFrameCompositorWindow();
[[nodiscard]] bool ReinitializeAcceleratedFrameCompositor(SDL_Window *window);
[[nodiscard]] bool AcceleratedFrameCompositorIsActive();
void ShutdownAcceleratedFrameCompositor();
[[nodiscard]] std::unique_ptr<IFrameCompositorBackend> CreateAcceleratedFrameCompositorBackend(RenderFrameCompositorBackend backend);

} // namespace devilution
