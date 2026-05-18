#pragma once

#include <array>
#include <cstdint>

namespace devilution {

constexpr int PlayerNameLength = 32;

/** Walking directions */
enum {
	// clang-format off
	WALK_NE   =  1,
	WALK_NW   =  2,
	WALK_SE   =  3,
	WALK_SW   =  4,
	WALK_N    =  5,
	WALK_E    =  6,
	WALK_S    =  7,
	WALK_W    =  8,
	WALK_NONE = -1,
	// clang-format on
};

enum PLR_MODE : uint8_t {
	PM_STAND,
	PM_WALK_NORTHWARDS,
	PM_WALK_SOUTHWARDS,
	PM_WALK_SIDEWAYS,
	PM_ATTACK,
	PM_RATTACK,
	PM_BLOCK,
	PM_GOTHIT,
	PM_DEATH,
	PM_SPELL,
	PM_NEWLVL,
	PM_QUIT,
};

enum action_id : int8_t {
	// clang-format off
	ACTION_WALK        = -2, // Automatic walk when using gamepad
	ACTION_NONE        = -1,
	ACTION_ATTACK      = 9,
	ACTION_RATTACK     = 10,
	ACTION_SPELL       = 12,
	ACTION_OPERATE     = 13,
	ACTION_DISARM      = 14,
	ACTION_PICKUPITEM  = 15, // put item in hand (inventory screen open)
	ACTION_PICKUPAITEM = 16, // put item in inventory
	ACTION_TALK        = 17,
	ACTION_OPERATETK   = 18, // operate via telekinesis
	ACTION_ATTACKMON   = 20,
	ACTION_ATTACKPLR   = 21,
	ACTION_RATTACKMON  = 22,
	ACTION_RATTACKPLR  = 23,
	ACTION_SPELLMON    = 24,
	ACTION_SPELLPLR    = 25,
	ACTION_SPELLWALL   = 26,
	// clang-format on
};

enum class player_graphic : uint8_t {
	Stand,
	Walk,
	Attack,
	Hit,
	Lightning,
	Fire,
	Magic,
	Death,
	Block,

	LAST = Block
};

enum class PlayerWeaponGraphic : uint8_t {
	Unarmed,
	UnarmedShield,
	Sword,
	SwordShield,
	Bow,
	Axe,
	Mace,
	MaceShield,
	Staff,
};

/* @brief When the player dies, what is the reason/source why? */
enum class DeathReason {
	/* @brief Monster or Trap (dungeon) */
	MonsterOrTrap,
	/* @brief Other player or selfkill (for example firewall) */
	Player,
	/* @brief HP is zero but we don't know when or where this happened */
	Unknown,
};

/** Maps from armor animation to letter used in graphic files. */
constexpr std::array<char, 3> ArmourChar = {
	'l', // light
	'm', // medium
	'h', // heavy
};

/** Maps from weapon animation to letter used in graphic files. */
constexpr std::array<char, 9> WepChar = {
	'n', // unarmed
	'u', // no weapon + shield
	's', // sword + no shield
	'd', // sword + shield
	'b', // bow
	'a', // axe
	'm', // blunt + no shield
	'h', // blunt + shield
	't', // staff
};

} // namespace devilution
