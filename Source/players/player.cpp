/**
 * @file players/player.cpp
 *
 * Core Player member implementations that do not belong to the main player
 * action state machine.
 */

#include "player.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

#include "effects.h"
#include "engine/backbuffer_state.hpp"
#include "engine/random.hpp"
#include "inv_iterators.hpp"
#include "items/validation.h"
#include "levels/gendung.h"
#include "msg.h"
#include "players/movement.hpp"
#include "spells.h"
#include "tables/playerdat.hpp"
#include "utils/is_of.hpp"

namespace devilution {

void Player::CalcScrolls()
{
	_pScrlSpells = 0;
	for (const Item &item : InventoryAndBeltPlayerItemsRange { *this }) {
		if (item.isScroll() && item._iStatFlag) {
			_pScrlSpells |= GetSpellBitmask(item._iSpell);
		}
	}
	EnsureValidReadiedSpell(*this);
}

bool Player::CanUseItem(const Item &item) const
{
	if (!IsItemValid(*this, item))
		return false;

	return _pStrength >= item._iMinStr
	    && _pMagic >= item._iMinMag
	    && _pDexterity >= item._iMinDex;
}

void Player::RemoveInvItem(int iv, bool calcScrolls)
{
	if (this == MyPlayer) {
		// Locate the first grid index containing this item and notify remote clients
		for (size_t i = 0; i < InventoryGridCells; i++) {
			const int8_t itemIndex = InvGrid[i];
			if (std::abs(itemIndex) - 1 == iv) {
				NetSendCmdParam1(false, CMD_DELINVITEMS, static_cast<uint16_t>(i));
				break;
			}
		}
	}

	// Iterate through invGrid and remove every reference to item
	for (int8_t &itemIndex : InvGrid) {
		if (std::abs(itemIndex) - 1 == iv) {
			itemIndex = 0;
		}
	}

	InvList[iv].clear();

	_pNumInv--;

	// If the item at the end of inventory array isn't the one removed, shift all following items back one index to retain inventory order.
	if (_pNumInv > 0 && _pNumInv != iv) {
		for (int newIndex = iv; newIndex < _pNumInv; newIndex++) {
			InvList[newIndex] = InvList[newIndex + 1].pop();
		}

		for (int8_t &itemIndex : InvGrid) {
			if (itemIndex > iv + 1) { // if item was shifted, decrease the index so it's paired with the correct item.
				itemIndex--;
			}
			if (itemIndex < -(iv + 1)) {
				itemIndex++; // since occupied cells are negative, increment the index to keep it same as as top-left cell for item, only negative.
			}
		}
	}

	if (calcScrolls) {
		CalcScrolls();
	}
}

void Player::RemoveSpdBarItem(int iv)
{
	if (this == MyPlayer) {
		NetSendCmdParam1(false, CMD_DELBELTITEMS, iv);
	}

	SpdList[iv].clear();

	CalcScrolls();
	RedrawEverything();
}

[[nodiscard]] uint8_t Player::getId() const
{
	return static_cast<uint8_t>(std::distance<const Player *>(&Players[0], this));
}

int Player::GetBaseAttributeValue(CharacterAttribute attribute) const
{
	switch (attribute) {
	case CharacterAttribute::Dexterity:
		return this->_pBaseDex;
	case CharacterAttribute::Magic:
		return this->_pBaseMag;
	case CharacterAttribute::Strength:
		return this->_pBaseStr;
	case CharacterAttribute::Vitality:
		return this->_pBaseVit;
	default:
		app_fatal("Unsupported attribute");
	}
}

int Player::GetCurrentAttributeValue(CharacterAttribute attribute) const
{
	switch (attribute) {
	case CharacterAttribute::Dexterity:
		return this->_pDexterity;
	case CharacterAttribute::Magic:
		return this->_pMagic;
	case CharacterAttribute::Strength:
		return this->_pStrength;
	case CharacterAttribute::Vitality:
		return this->_pVitality;
	default:
		app_fatal("Unsupported attribute");
	}
}

int Player::GetMaximumAttributeValue(CharacterAttribute attribute) const
{
	const ClassAttributes &attr = getClassAttributes();
	switch (attribute) {
	case CharacterAttribute::Strength:
		return attr.maxStr;
	case CharacterAttribute::Magic:
		return attr.maxMag;
	case CharacterAttribute::Dexterity:
		return attr.maxDex;
	case CharacterAttribute::Vitality:
		return attr.maxVit;
	}
	app_fatal("Unsupported attribute");
}

Point Player::GetTargetPosition() const
{
	// clang-format off
	constexpr int DirectionOffsetX[8] = {  0,-1, 1, 0,-1, 1, 1,-1 };
	constexpr int DirectionOffsetY[8] = { -1, 0, 0, 1,-1,-1, 1, 1 };
	// clang-format on
	Point target = position.future;
	for (auto step : walkpath) {
		if (step == WALK_NONE)
			break;
		if (step > 0) {
			target.x += DirectionOffsetX[step - 1];
			target.y += DirectionOffsetY[step - 1];
		}
	}
	return target;
}

int Player::GetPositionPathIndex(Point pos)
{
	constexpr Displacement DirectionOffset[8] = { { 0, -1 }, { -1, 0 }, { 1, 0 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	Point target = position.future;
	int i = 0;
	for (auto step : walkpath) {
		if (target == pos) return i;
		if (step == WALK_NONE)
			break;
		if (step > 0) {
			target += DirectionOffset[step - 1];
		}
		++i;
	}
	return -1;
}

void Player::Say(HeroSpeech speechId) const
{
	const SfxID soundEffect = GetHeroSound(_pClass, speechId);

	if (soundEffect == SfxID::None)
		return;

	PlaySfxLoc(soundEffect, position.tile);
}

void Player::SaySpecific(HeroSpeech speechId) const
{
	const SfxID soundEffect = GetHeroSound(_pClass, speechId);

	if (soundEffect == SfxID::None || effect_is_playing(soundEffect))
		return;

	PlaySfxLoc(soundEffect, position.tile, false);
}

void Player::Say(HeroSpeech speechId, int delay) const
{
	sfxdelay = delay;
	sfxdnum = GetHeroSound(_pClass, speechId);
}

void Player::Stop()
{
	ClrPlrPath(*this);
	destAction = ACTION_NONE;
}

bool Player::isWalking() const
{
	return IsAnyOf(_pmode, PM_WALK_NORTHWARDS, PM_WALK_SOUTHWARDS, PM_WALK_SIDEWAYS);
}

int Player::GetManaShieldDamageReduction()
{
	constexpr uint8_t Max = 7;
	return 24 - (std::min(_pSplLvl[static_cast<int8_t>(SpellID::ManaShield)], Max) * 3);
}

void Player::RestorePartialLife()
{
	const int wholeHitpoints = _pMaxHP >> 6;
	int l = ((wholeHitpoints / 8) + GenerateRnd(wholeHitpoints / 4)) << 6;
	if (IsAnyOf(_pClass, HeroClass::Warrior, HeroClass::Barbarian))
		l *= 2;
	if (IsAnyOf(_pClass, HeroClass::Rogue, HeroClass::Monk, HeroClass::Bard))
		l += l / 2;
	_pHitPoints = std::min(_pHitPoints + l, _pMaxHP);
	_pHPBase = std::min(_pHPBase + l, _pMaxHPBase);
}

void Player::RestorePartialMana()
{
	const int wholeManaPoints = _pMaxMana >> 6;
	int l = ((wholeManaPoints / 8) + GenerateRnd(wholeManaPoints / 4)) << 6;
	if (_pClass == HeroClass::Sorcerer)
		l *= 2;
	if (IsAnyOf(_pClass, HeroClass::Rogue, HeroClass::Monk, HeroClass::Bard))
		l += l / 2;
	if (HasNoneOf(_pIFlags, ItemSpecialEffect::NoMana)) {
		_pMana = std::min(_pMana + l, _pMaxMana);
		_pManaBase = std::min(_pManaBase + l, _pMaxManaBase);
	}
}

void Player::ReadySpellFromEquipment(inv_body_loc bodyLocation, bool forceSpell)
{
	const Item &item = InvBody[bodyLocation];
	if (item._itype == ItemType::Staff && IsValidSpell(item._iSpell) && item._iCharges > 0 && item._iStatFlag) {
		if (forceSpell || _pRSpell == SpellID::Invalid || _pRSplType == SpellType::Invalid) {
			_pRSpell = item._iSpell;
			_pRSplType = SpellType::Charges;
			RedrawEverything();
		}
	}
}

void Player::setCharacterLevel(uint8_t level)
{
	this->_pLevel = std::clamp<uint8_t>(level, 1U, getMaxCharacterLevel());
}

uint8_t Player::getMaxCharacterLevel() const
{
	return GetMaximumCharacterLevel();
}

uint32_t Player::getNextExperienceThreshold() const
{
	return GetNextExperienceThresholdForLevel(this->getCharacterLevel());
}

int32_t Player::calculateBaseLife() const
{
	const ClassAttributes &attr = getClassAttributes();
	return attr.adjLife + (attr.lvlLife * getCharacterLevel()) + (attr.chrLife * _pBaseVit);
}

int32_t Player::calculateBaseMana() const
{
	const ClassAttributes &attr = getClassAttributes();
	return attr.adjMana + (attr.lvlMana * getCharacterLevel()) + (attr.chrMana * _pBaseMag);
}

void Player::occupyTile(Point tilePosition, bool isMoving) const
{
	int16_t id = this->getId();
	id += 1;
	dPlayer[tilePosition.x][tilePosition.y] = isMoving ? -id : id;
}

bool Player::isLevelOwnedByLocalClient() const
{
	for (const Player &other : Players) {
		if (!other.plractive)
			continue;
		if (other._pLvlChanging)
			continue;
		if (other._pmode == PM_NEWLVL)
			continue;
		if (other.plrlevel != this->plrlevel)
			continue;
		if (other.plrIsOnSetLevel != this->plrIsOnSetLevel)
			continue;
		if (&other == MyPlayer && gbBufferMsgs != 0)
			continue;
		return &other == MyPlayer;
	}

	return false;
}

} // namespace devilution
