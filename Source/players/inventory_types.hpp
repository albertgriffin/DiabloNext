#pragma once

#include <cstdint>

namespace devilution {

constexpr int InventoryGridCells = 80;
constexpr int MaxBeltItems = 8;

// Logical equipment locations.
enum inv_body_loc : uint8_t {
	INVLOC_HEAD,
	INVLOC_RING_LEFT,
	INVLOC_RING_RIGHT,
	INVLOC_AMULET,
	INVLOC_HAND_LEFT,
	INVLOC_HAND_RIGHT,
	INVLOC_CHEST,
	NUM_INVLOC,
};

} // namespace devilution
