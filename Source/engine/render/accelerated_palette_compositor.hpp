/**
 * @file accelerated_palette_compositor.hpp
 *
 * Production-facing accelerated palette compositor seam.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#ifdef USE_SDL3
#include <SDL3/SDL_surface.h>
#else
#include <SDL.h>
#endif

#include "engine/render/frame_compositor.hpp"

namespace devilution {

enum class CompositionLightingBufferFormat : uint8_t {
	Rgba8,
	Alpha8,
};

struct CompositionLightingBufferView {
	const uint8_t *pixels = nullptr;
	Size size;
	int pitch = 0;
	CompositionLightingBufferFormat format = CompositionLightingBufferFormat::Rgba8;

	[[nodiscard]] bool IsValid() const
	{
		return pixels != nullptr && size.width > 0 && size.height > 0 && pitch > 0;
	}
};

struct CompositionLightingInputs {
	CompositionLightingBufferView light;
	CompositionLightingBufferView shadow;
};

class NeutralCompositionLightingInputs {
public:
	[[nodiscard]] const CompositionLightingInputs *Prepare(Size size);
	[[nodiscard]] const CompositionLightingInputs *Get() const;

private:
	Size size_ {};
	CompositionLightingInputs inputs_ {};
	std::vector<uint8_t> lightPixels_;
	std::vector<uint8_t> shadowPixels_;
};

struct AcceleratedPaletteFrame {
	const CompositionFrame &composition;
	const CompositionLightingInputs *lighting = nullptr;
};

class IAcceleratedPalettePresenter {
public:
	virtual ~IAcceleratedPalettePresenter() = default;

	[[nodiscard]] virtual std::string_view Name() const = 0;
	[[nodiscard]] virtual bool IsAvailable() const = 0;
	[[nodiscard]] virtual bool PrepareIndexedFrame(const AcceleratedPaletteFrame &frame) = 0;
	[[nodiscard]] virtual bool PrepareOutputSurfaceFrame(const AcceleratedPaletteFrame &frame, SDL_Surface &outputSurface) = 0;
	virtual void Present() = 0;
};

[[nodiscard]] bool AcceleratedPaletteFrameRequiresCpuPixels(const CompositionFrame &frame);
[[nodiscard]] const CompositionLightingInputs *PrepareNeutralCompositionLightingInputs(Size size);
[[nodiscard]] std::unique_ptr<IFrameCompositorBackend> CreateAcceleratedPaletteCompositorBackend(std::unique_ptr<IAcceleratedPalettePresenter> presenter, const CompositionLightingInputs *lightingInputs = nullptr);

} // namespace devilution
