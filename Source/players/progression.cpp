#include "player.h"

#include <algorithm>
#include <cstdint>
#include <limits>

#include "control/control.hpp"
#include "controls/control_mode.hpp"
#include "effects.h"
#include "engine/backbuffer_state.hpp"
#include "game_mode.hpp"
#include "items.h"
#include "lua/lua_event.hpp"
#include "msg.h"
#include "options.h"
#include "tables/playerdat.hpp"
#include "utils/is_of.hpp"

namespace devilution {

int CalcStatDiff(Player &player)
{
	int diff = 0;
	for (auto attribute : enum_values<CharacterAttribute>()) {
		diff += player.GetMaximumAttributeValue(attribute);
		diff -= player.GetBaseAttributeValue(attribute);
	}
	return diff;
}

void NextPlrLevel(Player &player)
{
	player.setCharacterLevel(player.getCharacterLevel() + 1);

	CalcPlrInv(player, true);

	if (CalcStatDiff(player) < 5) {
		player._pStatPts = CalcStatDiff(player);
	} else {
		player._pStatPts += 5;
	}
	const int hp = player.getClassAttributes().lvlLife;

	player._pMaxHP += hp;
	player._pHitPoints = player._pMaxHP;
	player._pMaxHPBase += hp;
	player._pHPBase = player._pMaxHPBase;

	if (&player == MyPlayer) {
		RedrawComponent(PanelDrawComponent::Health);
	}

	const int mana = player.getClassAttributes().lvlMana;

	player._pMaxMana += mana;
	player._pMaxManaBase += mana;

	if (HasNoneOf(player._pIFlags, ItemSpecialEffect::NoMana)) {
		player._pMana = player._pMaxMana;
		player._pManaBase = player._pMaxManaBase;
	}

	if (&player == MyPlayer) {
		RedrawComponent(PanelDrawComponent::Mana);
	}

	if (ControlMode != ControlTypes::KeyboardAndMouse)
		FocusOnCharInfo();

	CalcPlrInv(player, true);
	PlaySFX(SfxID::ItemArmor);
	PlaySFX(SfxID::ItemSign);
}

void Player::_addExperience(uint32_t experience, int levelDelta)
{
	if (this != MyPlayer || hasNoLife())
		return;

	if (isMaxCharacterLevel()) {
		return;
	}

	// Adjust xp based on difference between the players current level and the target level (usually a monster level)
	uint32_t clampedExp = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(experience * (1 + levelDelta / 10.0)), 0, std::numeric_limits<uint32_t>::max()));

	// Prevent power leveling
	if (gbIsMultiplayer) {
		// for low level characters experience gain is capped to 1/20 of current levels xp
		// for high level characters experience gain is capped to 200 * current level - this is a smaller value than 1/20 of the exp needed for the next level after level 5.
		clampedExp = std::min<uint32_t>({ clampedExp, /* level 1-5: */ getNextExperienceThreshold() / 20U, /* level 6-50: */ 200U * getCharacterLevel() });
	}

	lua::OnPlayerGainExperience(this, clampedExp);

	const uint32_t maxExperience = GetNextExperienceThresholdForLevel(getMaxCharacterLevel());

	// ensure we only add enough experience to reach the max experience cap so we don't overflow
	_pExperience += std::min(clampedExp, maxExperience - _pExperience);

	if (*GetOptions().Gameplay.experienceBar) {
		RedrawEverything();
	}

	// Increase player level if applicable
	while (!isMaxCharacterLevel() && _pExperience >= getNextExperienceThreshold()) {
		// NextPlrLevel increments character level which changes the next experience threshold
		NextPlrLevel(*this);
	}

	NetSendCmdParam1(false, CMD_PLRLEVEL, getCharacterLevel());
}

void AddPlrMonstExper(int lvl, unsigned exp, char pmask)
{
	unsigned totplrs = 0;
	for (size_t i = 0; i < Players.size(); i++) {
		if (((1 << i) & pmask) != 0) {
			totplrs++;
		}
	}

	if (totplrs != 0) {
		const unsigned e = exp / totplrs;
		if ((pmask & (1 << MyPlayerId)) != 0)
			MyPlayer->addExperience(e, lvl);
	}
}

void CheckStats(Player &player)
{
	for (auto attribute : enum_values<CharacterAttribute>()) {
		const int maxStatPoint = player.GetMaximumAttributeValue(attribute);
		switch (attribute) {
		case CharacterAttribute::Strength:
			player._pBaseStr = std::clamp(player._pBaseStr, 0, maxStatPoint);
			break;
		case CharacterAttribute::Magic:
			player._pBaseMag = std::clamp(player._pBaseMag, 0, maxStatPoint);
			break;
		case CharacterAttribute::Dexterity:
			player._pBaseDex = std::clamp(player._pBaseDex, 0, maxStatPoint);
			break;
		case CharacterAttribute::Vitality:
			player._pBaseVit = std::clamp(player._pBaseVit, 0, maxStatPoint);
			break;
		}
	}
}

void ModifyPlrStr(Player &player, int l)
{
	l = std::clamp(l, 0 - player._pBaseStr, player.GetMaximumAttributeValue(CharacterAttribute::Strength) - player._pBaseStr);

	player._pStrength += l;
	player._pBaseStr += l;

	CalcPlrInv(player, true);

	if (&player == MyPlayer) {
		NetSendCmdParam1(false, CMD_SETSTR, player._pBaseStr);
	}
}

void ModifyPlrMag(Player &player, int l)
{
	l = std::clamp(l, 0 - player._pBaseMag, player.GetMaximumAttributeValue(CharacterAttribute::Magic) - player._pBaseMag);

	player._pMagic += l;
	player._pBaseMag += l;

	int ms = l;
	ms *= player.getClassAttributes().chrMana;

	player._pMaxManaBase += ms;
	player._pMaxMana += ms;
	if (HasNoneOf(player._pIFlags, ItemSpecialEffect::NoMana)) {
		player._pManaBase += ms;
		player._pMana += ms;
	}

	CalcPlrInv(player, true);

	if (&player == MyPlayer) {
		NetSendCmdParam1(false, CMD_SETMAG, player._pBaseMag);
	}
}

void ModifyPlrDex(Player &player, int l)
{
	l = std::clamp(l, 0 - player._pBaseDex, player.GetMaximumAttributeValue(CharacterAttribute::Dexterity) - player._pBaseDex);

	player._pDexterity += l;
	player._pBaseDex += l;
	CalcPlrInv(player, true);

	if (&player == MyPlayer) {
		NetSendCmdParam1(false, CMD_SETDEX, player._pBaseDex);
	}
}

void ModifyPlrVit(Player &player, int l)
{
	l = std::clamp(l, 0 - player._pBaseVit, player.GetMaximumAttributeValue(CharacterAttribute::Vitality) - player._pBaseVit);

	player._pVitality += l;
	player._pBaseVit += l;

	int ms = l;
	ms *= player.getClassAttributes().chrLife;

	player._pHPBase += ms;
	player._pMaxHPBase += ms;
	player._pHitPoints += ms;
	player._pMaxHP += ms;

	CalcPlrInv(player, true);

	if (&player == MyPlayer) {
		NetSendCmdParam1(false, CMD_SETVIT, player._pBaseVit);
	}
}

void SetPlrStr(Player &player, int v)
{
	player._pBaseStr = v;
	CalcPlrInv(player, true);
}

void SetPlrMag(Player &player, int v)
{
	player._pBaseMag = v;

	int m = v;
	m *= player.getClassAttributes().chrMana;

	player._pMaxManaBase = m;
	player._pMaxMana = m;
	CalcPlrInv(player, true);
}

void SetPlrDex(Player &player, int v)
{
	player._pBaseDex = v;
	CalcPlrInv(player, true);
}

void SetPlrVit(Player &player, int v)
{
	player._pBaseVit = v;

	int hp = v;
	hp *= player.getClassAttributes().chrLife;

	player._pHPBase = hp;
	player._pMaxHPBase = hp;
	CalcPlrInv(player, true);
}

} // namespace devilution
