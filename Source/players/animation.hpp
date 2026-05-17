#pragma once

#include <cstdint>

#include "engine/animationinfo.h"
#include "engine/clx_sprite.hpp"
#include "engine/direction.hpp"
#include "players/player_types.hpp"
#include "tables/spelldat.h"

namespace devilution {

struct Player;

player_graphic GetPlayerGraphicForSpell(SpellID spellId);
ClxSprite GetPlayerPortraitSprite(Player &player);
bool IsPlayerUnarmed(Player &player);

void LoadPlrGFX(Player &player, player_graphic graphic);
void InitPlayerGFX(Player &player);
void ResetPlayerGFX(Player &player);
void NewPlrAnim(Player &player, player_graphic graphic, Direction dir, AnimationDistributionFlags flags = AnimationDistributionFlags::None, int8_t numSkippedFrames = 0, int8_t distributeFramesBeforeFrame = 0);
void SetPlrAnims(Player &player);

} // namespace devilution
