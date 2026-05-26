/**
 * @file render_layer_diagnostics.hpp
 *
 * Shared types for render layer diagnostics.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/displacement.hpp"
#include "engine/point.hpp"
#include "engine/rectangle.hpp"

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

enum class RenderWorldProxyDiagnosticMode : uint8_t {
	Off,
	Depth,
	Height,
	Receiver,
	Occluder,
	Type,
	Coverage,
	Outline,
};

enum class RenderWorldProxyPrimitive : uint8_t {
	FloorDiamond,
	LeftWallQuad,
	RightWallQuad,
	DoorArchBlocker,
	ObjectBlocker,
	ActorBillboard,
	Count,
};

inline constexpr uint8_t RenderWorldMaskReceiver = 1 << 0;
inline constexpr uint8_t RenderWorldMaskOccluder = 1 << 1;
inline constexpr uint8_t RenderWorldMaskEmissive = 1 << 2;

struct RenderLayerMapView {
	const uint8_t *pixels = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0;
	const Rectangle *dirtyRects = nullptr;
	size_t dirtyRectCount = 0;
	bool dirtyFullFrame = false;
	uint64_t version = 0;
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

struct RenderWorldProxyMapView {
	const uint8_t *typePixels = nullptr;
	const uint8_t *depthPixels = nullptr;
	const uint8_t *heightPixels = nullptr;
	const uint8_t *receiverPixels = nullptr;
	const uint8_t *occluderPixels = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0;
	uint64_t version = 0;
};

struct RenderClassicLightMapView {
	const uint8_t *lightLevelPixels = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0;
	uint64_t version = 0;
	bool storesIntensity = false;
	bool storesDungeonGrid = false;
	Point firstTile {};
	Displacement offset {};
	int viewportHeight = 0;
};

struct MutableRenderClassicLightMapView {
	uint8_t *pixels = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0;
	bool storesIntensity = false;
};

struct RenderSmoothLightSource {
	Point screenPosition;
	uint8_t radius = 0;
	uint8_t centerLightLevel = 0;
	uint8_t edgeLightLevel = 15;
};

struct RenderSmoothLightSourceView {
	const RenderSmoothLightSource *sources = nullptr;
	size_t count = 0;
	uint64_t version = 0;
};

inline constexpr uint8_t UnknownRenderLayerId = 0xFF;
inline constexpr uint8_t UnknownRenderWorldMaterialId = static_cast<uint8_t>(RenderWorldMaterial::Unknown);
inline constexpr uint8_t UnknownRenderWorldProxyPrimitiveId = 0xFF;
inline constexpr uint8_t FullyLitRenderClassicLightLevel = 0;
inline constexpr uint8_t NonWorldRenderClassicLightLevel = 0xFF;

} // namespace devilution
