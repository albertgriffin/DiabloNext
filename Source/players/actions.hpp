#pragma once

#include "tables/spelldat.h"

namespace devilution {

void ProcessPlayers();
void CheckPlrSpell(bool isShiftHeld);
void CheckPlrSpell(bool isShiftHeld, SpellID spellID, SpellType spellType);

} // namespace devilution
