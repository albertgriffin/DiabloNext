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

enum class RenderWorldMaterial : uint8_t {
	Unknown,
	Floor,
	LeftWall,
	RightWall,
	DoorArchBlocker,
	Actor,
	Object,
	Missile,
	Item,
	Corpse,
	SpecialSurface,
	Count,
};

enum class RenderWorldMaskDiagnosticMode : uint8_t {
	Off,
	Material,
	Receiver,
	Occluder,
	Emissive,
};

inline constexpr uint8_t RenderWorldMaskReceiver = 1 << 0;
inline constexpr uint8_t RenderWorldMaskOccluder = 1 << 1;
inline constexpr uint8_t RenderWorldMaskEmissive = 1 << 2;

struct RenderLayerMapView {
	const uint8_t *pixels = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0;
};

struct RenderWorldMaskMapView {
	const uint8_t *materialPixels = nullptr;
	const uint8_t *receiverPixels = nullptr;
	const uint8_t *occluderPixels = nullptr;
	const uint8_t *emissivePixels = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0;
	uint64_t version = 0;
};

inline constexpr uint8_t UnknownRenderLayerId = 0xFF;
inline constexpr uint8_t UnknownRenderWorldMaterialId = static_cast<uint8_t>(RenderWorldMaterial::Unknown);

} // namespace devilution
