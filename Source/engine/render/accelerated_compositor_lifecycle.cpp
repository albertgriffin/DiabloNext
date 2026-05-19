/**
 * @file accelerated_compositor_lifecycle.cpp
 *
 * Lifecycle routing for optional accelerated frame compositor backends.
 */
#include "engine/render/accelerated_compositor_lifecycle.hpp"

#include <array>
#include <memory>
#include <string_view>

#include "engine/render/frame_compositor.hpp"
#include "engine/render/opengl_palette_compositor.hpp"
#include "engine/render/sdl_gpu_palette_compositor.hpp"
#include "options.h"

namespace devilution {
namespace {

struct AcceleratedCompositorBackendDescriptor {
	RenderFrameCompositorBackend backend;
	AcceleratedCompositorApi api;
	bool (*requested)();
	bool (*windowRequested)();
	AcceleratedCompositorWindowFlags (*configureWindow)();
	bool (*reinitialize)(SDL_Window *window);
	bool (*active)();
	void (*shutdown)();
	std::unique_ptr<IFrameCompositorBackend> (*createBackend)();
};

std::unique_ptr<IFrameCompositorBackend> CreateNoAcceleratedBackend()
{
	return nullptr;
}

bool FalsePredicate()
{
	return false;
}

AcceleratedCompositorWindowFlags NoopConfigureWindow()
{
	return 0;
}

bool NoopReinitialize(SDL_Window *window)
{
	(void)window;
	return false;
}

void NoopShutdown()
{
}

constexpr std::array<AcceleratedCompositorBackendDescriptor, 3> AcceleratedCompositorBackends {
	{
	    {
	        RenderFrameCompositorBackend::CpuPalette,
	        AcceleratedCompositorApi::None,
	        FalsePredicate,
	        FalsePredicate,
	        NoopConfigureWindow,
	        NoopReinitialize,
	        FalsePredicate,
	        NoopShutdown,
	        CreateNoAcceleratedBackend,
	    },
	    {
	        RenderFrameCompositorBackend::OpenGlPalette,
	        AcceleratedCompositorApi::OpenGl,
	        OpenGlPaletteCompositorRequested,
	        OpenGlPaletteCompositorWindowRequested,
	        ConfigureOpenGlPaletteCompositorWindow,
	        ReinitializeOpenGlPaletteCompositor,
	        OpenGlPaletteCompositorIsActive,
	        ShutdownOpenGlPaletteCompositor,
	        CreateOpenGlPaletteCompositorBackend,
	    },
	    {
	        RenderFrameCompositorBackend::SdlGpuPalette,
	        AcceleratedCompositorApi::SdlGpu,
	        SdlGpuPaletteCompositorRequested,
	        SdlGpuPaletteCompositorWindowRequested,
	        ConfigureSdlGpuPaletteCompositorWindow,
	        ReinitializeSdlGpuPaletteCompositor,
	        SdlGpuPaletteCompositorIsActive,
	        ShutdownSdlGpuPaletteCompositor,
	        CreateSdlGpuPaletteCompositorBackend,
	    },
	},
};

const AcceleratedCompositorBackendDescriptor *FindAcceleratedCompositorBackend(const RenderFrameCompositorBackend backend)
{
	for (const AcceleratedCompositorBackendDescriptor &descriptor : AcceleratedCompositorBackends) {
		if (descriptor.backend == backend)
			return &descriptor;
	}
	return nullptr;
}

const AcceleratedCompositorBackendDescriptor *RequestedAcceleratedCompositorBackend()
{
	return FindAcceleratedCompositorBackend(*GetOptions().Experimental.renderFrameCompositorBackend);
}

} // namespace

std::string_view AcceleratedCompositorApiName(const AcceleratedCompositorApi api)
{
	switch (api) {
	case AcceleratedCompositorApi::None:
		return "none";
	case AcceleratedCompositorApi::OpenGl:
		return "OpenGL";
	case AcceleratedCompositorApi::SdlGpu:
		return "SDL_GPU";
	}
	return "unknown";
}

AcceleratedCompositorApi AcceleratedFrameCompositorRequestedApi()
{
	const AcceleratedCompositorBackendDescriptor *descriptor = RequestedAcceleratedCompositorBackend();
	if (descriptor == nullptr || !descriptor->requested())
		return AcceleratedCompositorApi::None;
	return descriptor->api;
}

AcceleratedCompositorApi AcceleratedFrameCompositorActiveApi()
{
	const AcceleratedCompositorBackendDescriptor *descriptor = RequestedAcceleratedCompositorBackend();
	if (descriptor == nullptr || !descriptor->active())
		return AcceleratedCompositorApi::None;
	return descriptor->api;
}

bool AcceleratedFrameCompositorRequested()
{
	return AcceleratedFrameCompositorRequestedApi() != AcceleratedCompositorApi::None;
}

bool AcceleratedFrameCompositorWindowRequested()
{
	const AcceleratedCompositorBackendDescriptor *descriptor = RequestedAcceleratedCompositorBackend();
	return descriptor != nullptr && descriptor->windowRequested();
}

AcceleratedCompositorWindowFlags ConfigureAcceleratedFrameCompositorWindow()
{
	const AcceleratedCompositorBackendDescriptor *descriptor = RequestedAcceleratedCompositorBackend();
	if (descriptor != nullptr)
		return descriptor->configureWindow();
	return 0;
}

bool ReinitializeAcceleratedFrameCompositor(SDL_Window *window)
{
	const AcceleratedCompositorBackendDescriptor *descriptor = RequestedAcceleratedCompositorBackend();
	return descriptor != nullptr && descriptor->reinitialize(window);
}

bool AcceleratedFrameCompositorIsActive()
{
	return AcceleratedFrameCompositorActiveApi() != AcceleratedCompositorApi::None;
}

void ShutdownAcceleratedFrameCompositor()
{
	for (const AcceleratedCompositorBackendDescriptor &descriptor : AcceleratedCompositorBackends) {
		descriptor.shutdown();
	}
}

std::unique_ptr<IFrameCompositorBackend> CreateAcceleratedFrameCompositorBackend(const RenderFrameCompositorBackend backend)
{
	const AcceleratedCompositorBackendDescriptor *descriptor = FindAcceleratedCompositorBackend(backend);
	if (descriptor == nullptr)
		return nullptr;
	return descriptor->createBackend();
}

} // namespace devilution
