#pragma once

#include <cstddef>
#include <cstdint>

#include "dvlnet/packet.h"

namespace devilution::net {

std::size_t GetPlayerCount();
void CopyActivePlayerName(plr_t playerId, uint8_t *destination);

} // namespace devilution::net
