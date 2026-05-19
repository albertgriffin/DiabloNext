/**
 * @file accelerated_compositor_lifecycle.cpp
 *
 * Lifecycle routing for optional accelerated frame compositor backends.
 */
#include "engine/render/accelerated_compositor_lifecycle.hpp"

#include <memory>

#include "engine/render/frame_compositor.hpp"
#include "engine/render/opengl_palette_compositor.hpp"
#include "options.h"

namespace devilution {

bool AcceleratedFrameCompositorRequested()
{
	switch (*GetOptions().Experimental.renderFrameCompositorBackend) {
	case RenderFrameCompositorBackend::OpenGlPalette:
		return OpenGlPaletteCompositorRequested();
	case RenderFrameCompositorBackend::CpuPalette:
		break;
	}
	return false;
}

bool AcceleratedFrameCompositorWindowRequested()
{
	switch (*GetOptions().Experimental.renderFrameCompositorBackend) {
	case RenderFrameCompositorBackend::OpenGlPalette:
		return OpenGlPaletteCompositorWindowRequested();
	case RenderFrameCompositorBackend::CpuPalette:
		break;
	}
	return false;
}

void ConfigureAcceleratedFrameCompositorWindow()
{
	switch (*GetOptions().Experimental.renderFrameCompositorBackend) {
	case RenderFrameCompositorBackend::OpenGlPalette:
		ConfigureOpenGlPaletteCompositorWindow();
		return;
	case RenderFrameCompositorBackend::CpuPalette:
		break;
	}
}

bool ReinitializeAcceleratedFrameCompositor(SDL_Window *window)
{
	switch (*GetOptions().Experimental.renderFrameCompositorBackend) {
	case RenderFrameCompositorBackend::OpenGlPalette:
		return ReinitializeOpenGlPaletteCompositor(window);
	case RenderFrameCompositorBackend::CpuPalette:
		break;
	}
	return false;
}

bool AcceleratedFrameCompositorIsActive()
{
	switch (*GetOptions().Experimental.renderFrameCompositorBackend) {
	case RenderFrameCompositorBackend::OpenGlPalette:
		return OpenGlPaletteCompositorIsActive();
	case RenderFrameCompositorBackend::CpuPalette:
		break;
	}
	return false;
}

void ShutdownAcceleratedFrameCompositor()
{
	ShutdownOpenGlPaletteCompositor();
}

std::unique_ptr<IFrameCompositorBackend> CreateAcceleratedFrameCompositorBackend(const RenderFrameCompositorBackend backend)
{
	switch (backend) {
	case RenderFrameCompositorBackend::OpenGlPalette:
		return CreateOpenGlPaletteCompositorBackend();
	case RenderFrameCompositorBackend::CpuPalette:
		break;
	}
	return nullptr;
}

} // namespace devilution
