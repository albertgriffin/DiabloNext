#pragma once

#include "engine/direction.hpp"
#include "engine/point.hpp"

namespace devilution {

struct Player;

void ClearStateVariables(Player &player);
bool PlayerCanUseFastWalk(const Player &player);
bool PlrDirOK(const Player &player, Direction dir);
void StartWalk(Player &player, Direction dir, bool pmWillBeCalled);
bool DoWalk(Player &player);
void SetPlayerOld(Player &player);
void FixPlayerLocation(Player &player, Direction bDir);
void StartStand(Player &player, Direction dir);
void FixPlrWalkTags(const Player &player);
void ClrPlrPath(Player &player);
bool PosOkPlayer(const Player &player, Point position);
void MakePlrPath(Player &player, Point targetPosition, bool endspace);

} // namespace devilution
