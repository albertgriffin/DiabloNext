#pragma once

#include "engine/direction.hpp"
#include "engine/world_tile.hpp"

namespace devilution {

struct Player;

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
bool PlayerIsInCombat(const Player &player);
void StartPlrBlock(Player &player, Direction dir);
void StartPlrHit(Player &player, int dam, bool forcehit);

} // namespace devilution
