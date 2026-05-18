#include "dvlnet/player_info.h"

#include <cstring>

#include "players/player_globals.hpp"

namespace devilution::net {

std::size_t GetPlayerCount()
{
	return PlayersCount();
}

void CopyActivePlayerName(plr_t playerId, uint8_t *destination)
{
	std::memset(destination, '\0', PlayerNameLength);

	if (!IsPlayerActive(playerId))
		return;

	std::memcpy(destination, GetPlayerName(playerId), PlayerNameLength);
}

} // namespace devilution::net
