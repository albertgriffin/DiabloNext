#pragma once

#include "players/player_types.hpp"
#include "tables/misdat.h"

namespace devilution {

struct Player;

bool DoDeath(Player &player);
bool PlrDeathModeOK(Player &player);
void ApplyPlrDamage(DamageType damageType, Player &player, int dam, int minHP = 0, int frac = 0, DeathReason deathReason = DeathReason::MonsterOrTrap);
void StartPlayerKill(Player &player, DeathReason deathReason);
void StripTopGold(Player &player);
void SyncPlrKill(Player &player, DeathReason deathReason);

} // namespace devilution
