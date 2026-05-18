#include "dvlnet/player_info.h"

#include <cstring>

#include "player.h"

namespace devilution::net {

std::size_t GetPlayerCount()
{
	return Players.size();
}

void CopyActivePlayerName(plr_t playerId, uint8_t *destination)
{
	std::memset(destination, '\0', PlayerNameLength);

	if (playerId >= Players.size())
		return;

	const Player &player = Players[playerId];
	if (!player.plractive)
		return;

	std::memcpy(destination, player._pName, PlayerNameLength);
}

} // namespace devilution::net
