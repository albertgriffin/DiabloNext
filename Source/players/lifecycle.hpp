#pragma once

#include <cstddef>

#include "interfac.h"
#include "tables/playerdat.hpp"

namespace devilution {

struct Player;

void CreatePlayer(Player &player, HeroClass c);
void InitPlayer(Player &player, bool firstTime);
void InitMultiView();
void StartNewLvl(Player &player, interface_mode fom, int lvl);
void RestartTownLvl(Player &player);
void StartWarpLvl(Player &player, size_t pidx);
void SyncPlrAnim(Player &player);
void SyncInitPlrPos(Player &player);
void SyncInitPlr(Player &player);

} // namespace devilution
