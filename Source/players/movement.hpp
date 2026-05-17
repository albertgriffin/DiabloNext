#pragma once

#include "player.h"

namespace devilution {

void ClearStateVariables(Player &player);
bool PlayerCanUseFastWalk(const Player &player);
bool PlrDirOK(const Player &player, Direction dir);
void StartWalk(Player &player, Direction dir, bool pmWillBeCalled);
bool DoWalk(Player &player);

} // namespace devilution
