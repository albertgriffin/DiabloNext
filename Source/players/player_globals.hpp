#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

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

/** @brief Do we currently inspect a remote player (/inspect was used)? In this case the (remote) players items and stats can't be modified. */
inline bool IsInspectingPlayer()
{
	return MyPlayer != InspectPlayer;
}

} // namespace devilution
