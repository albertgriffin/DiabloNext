/**
 * @file sdl_gpu_palette_compositor.cpp
 *
 * Optional SDL_GPU palette compositor target.
 */
#include "engine/render/sdl_gpu_palette_compositor.hpp"

#include <memory>

#ifdef USE_SDL3
#include <SDL3/SDL_video.h>
#endif

#include "controls/control_mode.hpp"
#include "engine/render/frame_compositor.hpp"
#include "headless_mode.hpp"
#include "options.h"
#include "utils/log.hpp"

#if defined(DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR) && defined(USE_SDL3) && !defined(USE_SDL1)
#include <SDL3/SDL_gpu.h>
#define DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE 1
#else
#define DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE 0
#endif

namespace devilution {
namespace {

[[nodiscard]] bool SdlGpuPaletteCompositorBuildAvailable()
{
	return DEVILUTIONX_SDL_GPU_PALETTE_COMPOSITOR_ACTIVE != 0;
}

[[nodiscard]] bool SdlGpuPaletteCompositorAllowedByRuntime()
{
#ifdef USE_SDL1
	return false;
#else
	return !HeadlessMode
	    && *GetOptions().Experimental.renderFrameCompositor
	    && *GetOptions().Experimental.renderFrameCompositorBackend == RenderFrameCompositorBackend::SdlGpuPalette
	    && ControlMode != ControlTypes::VirtualGamepad;
#endif
}

bool LoggedUnavailableBuild = false;
bool LoggedUnavailableImplementation = false;

} // namespace

bool SdlGpuPaletteCompositorRequested()
{
	return SdlGpuPaletteCompositorAllowedByRuntime();
}

bool SdlGpuPaletteCompositorWindowRequested()
{
	return false;
}

AcceleratedCompositorWindowFlags ConfigureSdlGpuPaletteCompositorWindow()
{
	return 0;
}

bool ReinitializeSdlGpuPaletteCompositor(SDL_Window *window)
{
	(void)window;
	if (SdlGpuPaletteCompositorAllowedByRuntime() && SdlGpuPaletteCompositorBuildAvailable() && !LoggedUnavailableImplementation) {
		Log("SDL_GPU palette compositor is selected but is not implemented yet; falling back to CPU palette compositor");
		LoggedUnavailableImplementation = true;
	}
	return false;
}

bool SdlGpuPaletteCompositorIsActive()
{
	return false;
}

void ShutdownSdlGpuPaletteCompositor()
{
}

std::unique_ptr<IFrameCompositorBackend> CreateSdlGpuPaletteCompositorBackend()
{
	if (SdlGpuPaletteCompositorAllowedByRuntime() && !SdlGpuPaletteCompositorBuildAvailable() && !LoggedUnavailableBuild) {
		Log("SDL_GPU palette compositor was selected but is not built; falling back to CPU palette compositor");
		LoggedUnavailableBuild = true;
	}
	return nullptr;
}

} // namespace devilution
