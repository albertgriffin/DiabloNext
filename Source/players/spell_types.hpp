#pragma once

#include <cstddef>
#include <cstdint>

#include "utils/enum_traits.h"

namespace devilution {

constexpr uint8_t MaxSpellLevel = 15;
constexpr std::size_t NumHotkeys = 12;

enum class SpellFlag : uint8_t {
	// clang-format off
	None         = 0,
	Etherealize  = 1 << 0,
	RageActive   = 1 << 1,
	RageCooldown = 1 << 2,
	// bits 3-7 are unused
	// clang-format on
};
use_enum_as_flags(SpellFlag);

} // namespace devilution
