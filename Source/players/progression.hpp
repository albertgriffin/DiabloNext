#pragma once

namespace devilution {

struct Player;

int CalcStatDiff(Player &player);
#ifdef _DEBUG
void NextPlrLevel(Player &player);
#endif
void AddPlrMonstExper(int lvl, unsigned int exp, char pmask);
void CheckStats(Player &player);
void ModifyPlrStr(Player &player, int l);
void ModifyPlrMag(Player &player, int l);
void ModifyPlrDex(Player &player, int l);
void ModifyPlrVit(Player &player, int l);
void SetPlrStr(Player &player, int v);
void SetPlrMag(Player &player, int v);
void SetPlrDex(Player &player, int v);
void SetPlrVit(Player &player, int v);

} // namespace devilution
