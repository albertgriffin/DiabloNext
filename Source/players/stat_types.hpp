#pragma once

#include <cstdint>

namespace devilution {

constexpr int MaxResistance = 75;

enum class CharacterAttribute : uint8_t {
	Strength,
	Magic,
	Dexterity,
	Vitality,

	FIRST = Strength,
	LAST = Vitality
};

} // namespace devilution
