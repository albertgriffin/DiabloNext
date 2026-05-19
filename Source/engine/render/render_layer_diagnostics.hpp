/**
 * @file render_layer_diagnostics.hpp
 *
 * Shared types for render layer diagnostics.
 */
#pragma once

#include <cstdint>

namespace devilution {

enum class RenderLayerDiagnosticMode : uint8_t {
	Off,
	Tint,
	Outline,
	TintAndOutline,
};

struct RenderLayerMapView {
	const uint8_t *pixels = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0;
};

inline constexpr uint8_t UnknownRenderLayerId = 0xFF;

} // namespace devilution
