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
#include "players/item_iterators.hpp"
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

bool Player::CanCleave()
{
	switch (_pClass) {
	case HeroClass::Warrior:
	case HeroClass::Rogue:
	case HeroClass::Sorcerer:
		return false;
	case HeroClass::Monk:
		return isEquipped(ItemType::Staff);
	case HeroClass::Bard:
		return InvBody[INVLOC_HAND_LEFT]._itype == ItemType::Sword && InvBody[INVLOC_HAND_RIGHT]._itype == ItemType::Sword;
	case HeroClass::Barbarian:
		return isEquipped(ItemType::Axe) || (!isEquipped(ItemType::Shield) && (isEquipped(ItemType::Mace, true) || isEquipped(ItemType::Sword, true)));
	default:
		return false;
	}
}

bool Player::isEquipped(ItemType itemType, bool isTwoHanded)
{
	switch (itemType) {
	case ItemType::Sword:
	case ItemType::Axe:
	case ItemType::Bow:
	case ItemType::Mace:
	case ItemType::Shield:
	case ItemType::Staff:
		return (InvBody[INVLOC_HAND_LEFT]._itype == itemType && (!isTwoHanded || InvBody[INVLOC_HAND_LEFT]._iLoc == ILOC_TWOHAND))
		    || (InvBody[INVLOC_HAND_RIGHT]._itype == itemType && (!isTwoHanded || InvBody[INVLOC_HAND_LEFT]._iLoc == ILOC_TWOHAND));
	case ItemType::LightArmor:
	case ItemType::MediumArmor:
	case ItemType::HeavyArmor:
		return InvBody[INVLOC_CHEST]._itype == itemType;
	case ItemType::Helm:
		return InvBody[INVLOC_HEAD]._itype == itemType;
	case ItemType::Ring:
		return InvBody[INVLOC_RING_LEFT]._itype == itemType || InvBody[INVLOC_RING_RIGHT]._itype == itemType;
	case ItemType::Amulet:
		return InvBody[INVLOC_AMULET]._itype == itemType;
	default:
		return false;
	}
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

item_equip_type Player::GetItemLocation(const Item &item) const
{
	if (_pClass == HeroClass::Barbarian && item._iLoc == ILOC_TWOHAND && IsAnyOf(item._itype, ItemType::Sword, ItemType::Mace))
		return ILOC_ONEHAND;
	return item._iLoc;
}

int Player::GetArmor() const
{
	return _pIBonusAC + _pIAC + _pDexterity / 5;
}

int Player::GetMeleeToHit() const
{
	return getCharacterLevel() + _pDexterity / 2 + _pIBonusToHit + getPlayerCombatData().baseMeleeToHit;
}

int Player::GetMeleePiercingToHit() const
{
	int hper = GetMeleeToHit();
	// in hellfire armor piercing ignores % of enemy armor instead, no way to include it here
	if (!gbIsHellfire)
		hper += _pIEnAc;
	return hper;
}

int Player::GetRangedToHit() const
{
	return getCharacterLevel() + _pDexterity + _pIBonusToHit + getPlayerCombatData().baseRangedToHit;
}

int Player::GetRangedPiercingToHit() const
{
	int hper = GetRangedToHit();
	// in hellfire armor piercing ignores % of enemy armor instead, no way to include it here
	if (!gbIsHellfire)
		hper += _pIEnAc;
	return hper;
}

int Player::GetMagicToHit() const
{
	return _pMagic + getPlayerCombatData().baseMagicToHit;
}

int Player::GetBlockChance(bool useLevel) const
{
	int blkper = _pDexterity + getBaseToBlock();
	if (useLevel)
		blkper += getCharacterLevel() * 2;
	return blkper;
}

int Player::GetManaShieldDamageReduction()
{
	constexpr uint8_t Max = 7;
	return 24 - (std::min(_pSplLvl[static_cast<int8_t>(SpellID::ManaShield)], Max) * 3);
}

int Player::GetSpellLevel(SpellID spell) const
{
	if (spell == SpellID::Invalid || static_cast<std::size_t>(spell) >= sizeof(_pSplLvl)) {
		return 0;
	}

	return std::max<int>(_pISplLvlAdd + _pSplLvl[static_cast<std::size_t>(spell)], 0);
}

int Player::CalculateArmorPierce(int monsterArmor, bool isMelee) const
{
	int tmac = monsterArmor;
	if (_pIEnAc > 0) {
		if (gbIsHellfire) {
			int pIEnAc = _pIEnAc - 1;
			if (pIEnAc > 0)
				tmac >>= pIEnAc;
			else
				tmac -= tmac / 4;
		}
		if (isMelee && _pClass == HeroClass::Barbarian) {
			tmac -= monsterArmor / 8;
		}
	}
	if (tmac < 0)
		tmac = 0;

	return tmac;
}

int Player::UpdateHitPointPercentage()
{
	if (_pMaxHP <= 0) { // divide by zero guard
		_pHPPer = 0;
	} else {
		// Maximum achievable HP is approximately 1200. Diablo uses fixed point integers where the last 6 bits are
		// fractional values. This means that we will never overflow HP values normally by doing this multiplication
		// as the max value is representable in 17 bits and the multiplication result will be at most 23 bits
		_pHPPer = std::clamp(_pHitPoints * 81 / _pMaxHP, 0, 81); // hp should never be greater than maxHP but just in case
	}

	return _pHPPer;
}

int Player::UpdateManaPercentage()
{
	if (_pMaxMana <= 0) {
		_pManaPer = 0;
	} else {
		_pManaPer = std::clamp(_pMana * 81 / _pMaxMana, 0, 81);
	}

	return _pManaPer;
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

void Player::RestoreFullLife()
{
	_pHitPoints = _pMaxHP;
	_pHPBase = _pMaxHPBase;
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

void Player::RestoreFullMana()
{
	if (HasNoneOf(_pIFlags, ItemSpecialEffect::NoMana)) {
		_pMana = _pMaxMana;
		_pManaBase = _pMaxManaBase;
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

bool Player::UsesRangedWeapon() const
{
	return static_cast<PlayerWeaponGraphic>(_pgfxnum & 0xF) == PlayerWeaponGraphic::Bow;
}

bool Player::CanChangeAction()
{
	if (_pmode == PM_STAND)
		return true;
	if (_pmode == PM_ATTACK && AnimInfo.currentFrame >= _pAFNum)
		return true;
	if (_pmode == PM_RATTACK && AnimInfo.currentFrame >= _pAFNum)
		return true;
	if (_pmode == PM_SPELL && AnimInfo.currentFrame >= _pSFNum)
		return true;
	if (isWalking() && AnimInfo.isLastFrame())
		return true;
	return false;
}

ClxSprite Player::currentSprite() const
{
	return previewCelSprite ? *previewCelSprite : AnimInfo.currentSprite();
}

Displacement Player::getRenderingOffset(const ClxSprite sprite) const
{
	Displacement offset = { -CalculateSpriteTileCenterX(sprite.width()), 0 };
	if (isWalking())
		offset += GetOffsetForWalking(AnimInfo, _pdir);
	return offset;
}

uint8_t Player::getCharacterLevel() const
{
	return _pLevel;
}

void Player::setCharacterLevel(uint8_t level)
{
	this->_pLevel = std::clamp<uint8_t>(level, 1U, getMaxCharacterLevel());
}

uint8_t Player::getMaxCharacterLevel() const
{
	return GetMaximumCharacterLevel();
}

bool Player::isMaxCharacterLevel() const
{
	return getCharacterLevel() >= getMaxCharacterLevel();
}

uint32_t Player::getNextExperienceThreshold() const
{
	return GetNextExperienceThresholdForLevel(this->getCharacterLevel());
}

bool Player::isOnActiveLevel() const
{
	if (setlevel)
		return isOnLevel(setlvlnum);
	return isOnLevel(currlevel);
}

bool Player::isOnLevel(uint8_t level) const
{
	return !this->plrIsOnSetLevel && this->plrlevel == level;
}

bool Player::isOnLevel(_setlevels level) const
{
	return this->plrIsOnSetLevel && this->plrlevel == static_cast<uint8_t>(level);
}

bool Player::isOnArenaLevel() const
{
	return plrIsOnSetLevel && IsArenaLevel(static_cast<_setlevels>(plrlevel));
}

void Player::setLevel(uint8_t level)
{
	this->plrlevel = level;
	this->plrIsOnSetLevel = false;
}

void Player::setLevel(_setlevels level)
{
	this->plrlevel = static_cast<uint8_t>(level);
	this->plrIsOnSetLevel = true;
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

bool Player::isHoldingItem(const ItemType type) const
{
	const Item &leftHandItem = InvBody[INVLOC_HAND_LEFT];
	const Item &rightHandItem = InvBody[INVLOC_HAND_RIGHT];

	return (type == leftHandItem._itype && leftHandItem._iStatFlag) || (type == rightHandItem._itype && rightHandItem._iStatFlag);
}

bool Player::hasNoLife() const
{
	return leveltype == DTYPE_TOWN ? false : _pHitPoints >> 6 <= 0;
}

bool Player::hasNoMana() const
{
	return _pMana >> 6 <= 0;
}

} // namespace devilution
