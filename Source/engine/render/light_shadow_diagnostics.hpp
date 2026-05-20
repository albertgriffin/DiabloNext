/**
 * @file light_shadow_diagnostics.hpp
 *
 * Shared types for development light/shadow diagnostics.
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace devilution {

enum class RenderLightShadowDiagnosticMode : uint8_t {
	Off = 0,
	FinalLitOutput = 1,
	LightRgb = 2,
	ShadowAlpha = 3,
};

[[nodiscard]] inline std::string_view RenderLightShadowDiagnosticModeName(const RenderLightShadowDiagnosticMode mode)
{
	switch (mode) {
	case RenderLightShadowDiagnosticMode::Off:
		return "off";
	case RenderLightShadowDiagnosticMode::FinalLitOutput:
		return "final-lit-output";
	case RenderLightShadowDiagnosticMode::LightRgb:
		return "light-rgb";
	case RenderLightShadowDiagnosticMode::ShadowAlpha:
		return "shadow-alpha";
	}
	return "unknown";
}

} // namespace devilution
