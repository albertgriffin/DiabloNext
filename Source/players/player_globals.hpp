#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "players/player_types.hpp"
#include "utils/attributes.h"

namespace devilution {

struct Player;

extern DVL_API_FOR_TEST uint8_t MyPlayerId;
extern DVL_API_FOR_TEST Player *MyPlayer;
extern DVL_API_FOR_TEST std::vector<Player> Players;
/** @brief What Player items and stats should be displayed? Normally this is identical to MyPlayer but can differ when /inspect was used. */
extern Player *InspectPlayer;
extern bool MyPlayerIsDead;

size_t PlayersCount();
uint8_t LocalPlayerId();
bool HasLocalPlayer();
const Player &LocalPlayer();
Player &MutableLocalPlayer();
const Player &GetPlayer(size_t playerId);
Player &GetMutablePlayer(size_t playerId);
bool IsValidPlayerId(size_t playerId);
bool IsPlayerActive(size_t playerId);
bool IsPlayerFriendly(size_t playerId);
bool IsLocalPlayerId(size_t playerId);
bool IsLocalPlayer(const Player &player);
const char (&GetPlayerName(size_t playerId))[PlayerNameLength];

/** @brief Do we currently inspect a remote player (/inspect was used)? In this case the (remote) players items and stats can't be modified. */
inline bool IsInspectingPlayer()
{
	return MyPlayer != InspectPlayer;
}

} // namespace devilution
