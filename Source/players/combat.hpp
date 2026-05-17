#pragma once

#include "player.h"

namespace devilution {

void RefreshPlayerCombatCooldown(Player &player);
void UpdatePlayerCombatCooldown(Player &player);
void StartAttack(Player &player, Direction d, bool includesFirstFrame);
void StartRangeAttack(Player &player, Direction d, WorldTileCoord cx, WorldTileCoord cy, bool includesFirstFrame);
void StartSpell(Player &player, Direction d, WorldTileCoord cx, WorldTileCoord cy);
bool DoAttack(Player &player);
bool DoRangeAttack(Player &player);
bool DoBlock(Player &player);
bool DoSpell(Player &player);
bool DoGotHit(Player &player);

} // namespace devilution
