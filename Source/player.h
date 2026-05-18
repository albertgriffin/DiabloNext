/**
 * @file player.h
 *
 * Interface of player functionality, leveling, actions, creation, loading, etc.
 */
#pragma once

#include <cstdint>

#include <array>
#include <string>
#include <string_view>

#include "diablo.h"
#include "engine/actor_position.hpp"
#include "engine/animationinfo.h"
#include "engine/clx_sprite.hpp"
#include "engine/displacement.hpp"
#include "engine/path.h"
#include "engine/point.hpp"
#include "game_mode.hpp"
#include "interfac.h"
#include "items.h"
#include "items/validation.h"
#include "levels/dun_tile.hpp"
#include "levels/gendung.h"
#include "multi.h"
#include "players/player_globals.hpp"
#include "players/inventory_types.hpp"
#include "players/player_types.hpp"
#include "players/spell_types.hpp"
#include "players/stat_types.hpp"
#include "tables/playerdat.hpp"
#include "tables/spelldat.h"
#include "utils/attributes.h"
#include "utils/enum_traits.h"
#include "utils/is_of.hpp"

namespace devilution {

/**
 * @brief Contains Data (CelSprites) for a player graphic (player_graphic)
 */
struct PlayerAnimationData {
	/**
	 * @brief Sprite lists for each of the 8 directions.
	 */
	OptionalOwnedClxSpriteSheet sprites;

	[[nodiscard]] ClxSpriteList spritesForDirection(Direction direction) const
	{
		return (*sprites)[static_cast<size_t>(direction)];
	}
};

struct SpellCastInfo {
	SpellID spellId;
	SpellType spellType;
	/* @brief Inventory location for scrolls */
	int8_t spellFrom;
	/* @brief Used for spell level */
	int spellLevel;
};

struct Player {
	Player() = default;
	Player(Player &&) noexcept = default;
	Player &operator=(Player &&) noexcept = default;

	char _pName[PlayerNameLength];
	Item InvBody[NUM_INVLOC];
	Item InvList[InventoryGridCells];
	Item SpdList[MaxBeltItems];
	Item HoldItem;

	int lightId;

	int _pNumInv;
	int _pStrength;
	int _pBaseStr;
	int _pMagic;
	int _pBaseMag;
	int _pDexterity;
	int _pBaseDex;
	int _pVitality;
	int _pBaseVit;
	int _pStatPts;
	int _pDamageMod;
	int _pHPBase;
	int _pMaxHPBase;
	int _pHitPoints;
	int _pMaxHP;
	int _pHPPer;
	int _pManaBase;
	int _pMaxManaBase;
	int _pMana;
	int _pMaxMana;
	int _pManaPer;
	int _pIMinDam;
	int _pIMaxDam;
	int _pIAC;
	int _pIBonusDam;
	int _pIBonusToHit;
	int _pIBonusAC;
	int _pIBonusDamMod;
	int _pIGetHit;
	int _pIEnAc;
	int _pIFMinDam;
	int _pIFMaxDam;
	int _pILMinDam;
	int _pILMaxDam;
	uint32_t _pExperience;
	PLR_MODE _pmode;
	int8_t walkpath[MaxPathLengthPlayer];
	bool plractive;
	action_id destAction;
	int destParam1;
	int destParam2;
	int destParam3;
	int destParam4;
	uint16_t outOfCombatSpeedCooldownTicks;
	int _pGold;

	/**
	 * @brief Contains Information for current Animation
	 */
	AnimationInfo AnimInfo;
	/**
	 * @brief Contains a optional preview ClxSprite that is displayed until the current command is handled by the game logic
	 */
	OptionalClxSprite previewCelSprite;
	/**
	 * @brief Contains the progress to next game tick when previewCelSprite was set
	 */
	int8_t progressToNextGameTickWhenPreviewWasSet;
	/** @brief Bitmask using item_special_effect */
	ItemSpecialEffect _pIFlags;
	/**
	 * @brief Contains Data (Sprites) for the different Animations
	 */
	std::array<PlayerAnimationData, enum_size<player_graphic>::value> AnimationData;
	std::array<OptionalOwnedClxSpriteSheet, 2> PartyInfoSprites;
	std::array<std::string, 2> PartyInfoSpriteLocations;
	int8_t _pNFrames;
	int8_t _pWFrames;
	int8_t _pAFrames;
	int8_t _pAFNum;
	int8_t _pSFrames;
	int8_t _pSFNum;
	int8_t _pHFrames;
	int8_t _pDFrames;
	int8_t _pBFrames;
	int8_t InvGrid[InventoryGridCells];

	uint8_t plrlevel;
	bool plrIsOnSetLevel;
	ActorPosition position;
	Direction _pdir; // Direction faced by player (direction enum)
	HeroClass _pClass;

private:
	uint8_t _pLevel = 1; // Use get/setCharacterLevel to ensure this attribute stays within the accepted range

public:
	uint8_t _pgfxnum; // Bitmask indicating what variant of the sprite the player is using. The 3 lower bits define weapon (PlayerWeaponGraphic) and the higher bits define armour (starting with PlayerArmorGraphic)
	int8_t _pISplLvlAdd;
	/** @brief Specifies whether players are in non-PvP mode. */
	bool friendlyMode = true;

	/** @brief The next queued spell */
	SpellCastInfo queuedSpell;
	/** @brief The spell that is currently being cast */
	SpellCastInfo executedSpell;
	/* @brief Which spell should be executed with CURSOR_TELEPORT */
	SpellID inventorySpell;
	/* @brief Inventory location for scrolls with CURSOR_TELEPORT */
	int8_t spellFrom;
	SpellID _pRSpell;
	SpellType _pRSplType;
	SpellID _pSBkSpell;
	uint8_t _pSplLvl[64];
	/** @brief Bitmask of staff spell */
	uint64_t _pISpells;
	/** @brief Bitmask of learned spells */
	uint64_t _pMemSpells;
	/** @brief Bitmask of abilities */
	uint64_t _pAblSpells;
	/** @brief Bitmask of spells available via scrolls */
	uint64_t _pScrlSpells;
	SpellFlag _pSpellFlags;
	SpellID _pSplHotKey[NumHotkeys];
	SpellType _pSplTHotKey[NumHotkeys];
	bool _pBlockFlag;
	bool _pInvincible;
	int8_t _pLightRad;
	/** @brief True when the player is transitioning between levels */
	bool _pLvlChanging;

	int8_t _pArmorClass;
	int8_t _pMagResist;
	int8_t _pFireResist;
	int8_t _pLghtResist;
	bool _pInfraFlag;
	/** Player's direction when ending movement. Also used for casting direction of SpellID::FireWall. */
	Direction tempDirection;

	bool _pLvlVisited[NUMLEVELS];
	bool _pSLvlVisited[NUMLEVELS]; // only 10 used

	item_misc_id _pOilType;
	uint8_t pTownWarps;
	uint8_t pDungMsgs;
	uint8_t pLvlLoad;
	bool pManaShield;
	uint8_t pDungMsgs2;
	bool pOriginalCathedral;
	uint8_t pDiabloKillLevel;
	uint16_t wReflections;
	ItemSpecialEffectHf pDamAcFlags;

	[[nodiscard]] std::string_view name() const
	{
		return _pName;
	}

	/**
	 * @brief Convenience function to get the base stats/bonuses for this player's class
	 */
	[[nodiscard]] const ClassAttributes &getClassAttributes() const
	{
		return GetClassAttributes(_pClass);
	}

	[[nodiscard]] const PlayerCombatData &getPlayerCombatData() const
	{
		return GetPlayerCombatDataForClass(_pClass);
	}

	[[nodiscard]] const PlayerData &getPlayerData() const
	{
		return GetPlayerDataForClass(_pClass);
	}

	/**
	 * @brief Gets the translated name for the character's class
	 */
	[[nodiscard]] std::string_view getClassName() const
	{
		return _(getPlayerData().className);
	}

	[[nodiscard]] int getBaseToBlock() const
	{
		return getPlayerCombatData().baseToBlock;
	}

	void CalcScrolls();

	bool CanUseItem(const Item &item) const;

	bool CanCleave();

	bool isEquipped(ItemType itemType, bool isTwoHanded = false);

	/**
	 * @brief Remove an item from player inventory
	 * @param iv invList index of item to be removed
	 * @param calcScrolls If true, CalcScrolls() gets called after removing item
	 */
	void RemoveInvItem(int iv, bool calcScrolls = true);

	/**
	 * @brief Returns the network identifier for this player
	 */
	[[nodiscard]] uint8_t getId() const;

	void RemoveSpdBarItem(int iv);

	/**
	 * @brief Gets the most valuable item out of all the player's items that match the given predicate.
	 * @param itemPredicate The predicate used to match the items.
	 * @return The most valuable item out of all the player's items that match the given predicate, or 'nullptr' in case no
	 * matching items were found.
	 */
	template <typename TPredicate>
	const Item *GetMostValuableItem(const TPredicate &itemPredicate) const
	{
		const auto getMostValuableItem = [&itemPredicate](const Item *begin, const Item *end, const Item *mostValuableItem = nullptr) {
			for (const auto *item = begin; item < end; item++) {
				if (item->isEmpty() || !itemPredicate(*item)) {
					continue;
				}

				if (mostValuableItem == nullptr || item->_iIvalue > mostValuableItem->_iIvalue) {
					mostValuableItem = item;
				}
			}

			return mostValuableItem;
		};

		const Item *mostValuableItem = getMostValuableItem(SpdList, SpdList + MaxBeltItems);
		mostValuableItem = getMostValuableItem(InvBody, InvBody + inv_body_loc::NUM_INVLOC, mostValuableItem);
		mostValuableItem = getMostValuableItem(InvList, InvList + _pNumInv, mostValuableItem);

		return mostValuableItem;
	}

	/**
	 * @brief Gets the base value of the player's specified attribute.
	 * @param attribute The attribute to retrieve the base value for
	 * @return The base value for the requested attribute.
	 */
	int GetBaseAttributeValue(CharacterAttribute attribute) const;

	/**
	 * @brief Gets the current value of the player's specified attribute.
	 * @param attribute The attribute to retrieve the current value for
	 * @return The current value for the requested attribute.
	 */
	int GetCurrentAttributeValue(CharacterAttribute attribute) const;

	/**
	 * @brief Gets the maximum value of the player's specified attribute.
	 * @param attribute The attribute to retrieve the maximum value for
	 * @return The maximum value for the requested attribute.
	 */
	int GetMaximumAttributeValue(CharacterAttribute attribute) const;

	/**
	 * @brief Get the tile coordinates a player is moving to (if not moving, then it corresponds to current position).
	 */
	Point GetTargetPosition() const;

	/**
	 * @brief Returns the index of the given position in `walkpath`, or -1 if not found.
	 */
	int GetPositionPathIndex(Point position);

	/**
	 * @brief Says a speech line.
	 * @todo BUGFIX Prevent more than one speech to be played at a time (reject new requests).
	 */
	void Say(HeroSpeech speechId) const;
	/**
	 * @brief Says a speech line after a given delay.
	 * @param speechId The speech ID to say.
	 * @param delay Multiple of 50ms wait before starting the speech
	 */
	void Say(HeroSpeech speechId, int delay) const;
	/**
	 * @brief Says a speech line, without random variants.
	 */
	void SaySpecific(HeroSpeech speechId) const;

	/**
	 * @brief Attempts to stop the player from performing any queued up action. If the player is currently walking, his walking will
	 * stop as soon as he reaches the next tile. If any action was queued with the previous command (like targeting a monster,
	 * opening a chest, picking an item up, etc) this action will also be cancelled.
	 */
	void Stop();

	/**
	 * @brief Is the player currently walking?
	 */
	bool isWalking() const;

	/**
	 * @brief Returns item location taking into consideration barbarian's ability to hold two-handed maces and clubs in one hand.
	 */
	item_equip_type GetItemLocation(const Item &item) const;

	/**
	 * @brief Return player's armor value
	 */
	int GetArmor() const;

	/**
	 * @brief Return player's melee to hit value
	 */
	int GetMeleeToHit() const;

	/**
	 * @brief Return player's melee to hit value, including armor piercing
	 */
	int GetMeleePiercingToHit() const;

	/**
	 * @brief Return player's ranged to hit value
	 */
	int GetRangedToHit() const;

	int GetRangedPiercingToHit() const;

	/**
	 * @brief Return magic hit chance
	 */
	int GetMagicToHit() const;

	/**
	 * @brief Return block chance
	 * @param useLevel - indicate if player's level should be added to block chance (the only case where it isn't is blocking a trap)
	 */
	int GetBlockChance(bool useLevel = true) const;

	/**
	 * @brief Return reciprocal of the factor for calculating damage reduction due to Mana Shield.
	 *
	 * Valid only for players with Mana Shield spell level greater than zero.
	 */
	int GetManaShieldDamageReduction();

	/**
	 * @brief Gets the effective spell level for the player, considering item bonuses
	 * @param spell SpellID enum member identifying the spell
	 * @return effective spell level
	 */
	int GetSpellLevel(SpellID spell) const;

	/**
	 * @brief Return monster armor value after including player's armor piercing % (hellfire only)
	 * @param monsterArmor - monster armor before applying % armor pierce
	 * @param isMelee - indicates if it's melee or ranged combat
	 */
	int CalculateArmorPierce(int monsterArmor, bool isMelee) const;

	/**
	 * @brief Calculates the players current Hit Points as a percentage of their max HP and stores it for later reference
	 *
	 * The stored value is unused...
	 * @see _pHPPer
	 * @return The players current hit points as a percentage of their maximum (from 0 to 80%)
	 */
	int UpdateHitPointPercentage();

	int UpdateManaPercentage();

	/**
	 * @brief Restores between 1/8 (inclusive) and 1/4 (exclusive) of the players max HP (further adjusted by class).
	 *
	 * This determines a random amount of non-fractional life points to restore then scales the value based on the
	 *  player class. Warriors/barbarians get between 1/4 and 1/2 life restored per potion, rogue/monk/bard get 3/16
	 *  to 3/8, and sorcerers get the base amount.
	 */
	void RestorePartialLife();

	/**
	 * @brief Resets hp to maxHp
	 */
	void RestoreFullLife();

	/**
	 * @brief Restores between 1/8 (inclusive) and 1/4 (exclusive) of the players max Mana (further adjusted by class).
	 *
	 * This determines a random amount of non-fractional mana points to restore then scales the value based on the
	 *  player class. Sorcerers get between 1/4 and 1/2 mana restored per potion, rogue/monk/bard get 3/16 to 3/8,
	 *  and warrior/barbarian get the base amount. However if the player can't use magic due to an equipped item then
	 *  they get nothing.
	 */
	void RestorePartialMana();

	/**
	 * @brief Resets mana to maxMana (if the player can use magic)
	 */
	void RestoreFullMana();
	/**
	 * @brief Sets the readied spell to the spell in the specified equipment slot. Does nothing if the item does not have a valid spell.
	 * @param bodyLocation - the body location whose item will be checked for the spell.
	 * @param forceSpell - if true, always change active spell, if false, only when current spell slot is empty
	 */
	void ReadySpellFromEquipment(inv_body_loc bodyLocation, bool forceSpell);

	/**
	 * @brief Does the player currently have a ranged weapon equipped?
	 */
	bool UsesRangedWeapon() const;

	bool CanChangeAction();

	[[nodiscard]] player_graphic getGraphic() const;

	[[nodiscard]] uint16_t getSpriteWidth() const;

	void getAnimationFramesAndTicksPerFrame(player_graphic graphics, int8_t &numberOfFrames, int8_t &ticksPerFrame) const;

	[[nodiscard]] ClxSprite currentSprite() const;
	[[nodiscard]] Displacement getRenderingOffset(const ClxSprite sprite) const;

	/**
	 * @brief Updates previewCelSprite according to new requested command
	 * @param cmdId What command is requested
	 * @param point Point for the command
	 * @param wParam1 First Parameter
	 * @param wParam2 Second Parameter
	 */
	void UpdatePreviewCelSprite(_cmd_id cmdId, Point point, uint16_t wParam1, uint16_t wParam2);

	[[nodiscard]] uint8_t getCharacterLevel() const;

	/**
	 * @brief Sets the character level to the target level or nearest valid value.
	 * @param level New character level, will be clamped to the allowed range
	 */
	void setCharacterLevel(uint8_t level);

	[[nodiscard]] uint8_t getMaxCharacterLevel() const;

	[[nodiscard]] bool isMaxCharacterLevel() const;

private:
	void _addExperience(uint32_t experience, int levelDelta);

public:
	/**
	 * @brief Adds experience to the local player based on the current game mode
	 * @param experience base value to add, this will be adjusted to prevent power leveling in multiplayer games
	 */
	void addExperience(uint32_t experience)
	{
		_addExperience(experience, 0);
	}

	/**
	 * @brief Adds experience to the local player based on the difference between the monster level
	 * and current level, then also applying the power level cap in multiplayer games.
	 * @param experience base value to add, will be scaled up/down by the difference between player and monster level
	 * @param monsterLevel level of the monster that has rewarded this experience
	 */
	void addExperience(uint32_t experience, int monsterLevel)
	{
		_addExperience(experience, monsterLevel - getCharacterLevel());
	}

	[[nodiscard]] uint32_t getNextExperienceThreshold() const;

	/** @brief Checks if the player is on the same level as the local player (MyPlayer). */
	bool isOnActiveLevel() const;

	/** @brief Checks if the player is on the corresponding level. */
	bool isOnLevel(uint8_t level) const;
	/** @brief Checks if the player is on the corresponding level. */
	bool isOnLevel(_setlevels level) const;
	/** @brief Checks if the player is on a arena level. */
	bool isOnArenaLevel() const;
	void setLevel(uint8_t level);
	void setLevel(_setlevels level);

	/** @brief Returns a character's life based on starting life, character level, and base vitality. */
	int32_t calculateBaseLife() const;

	/** @brief Returns a character's mana based on starting mana, character level, and base magic. */
	int32_t calculateBaseMana() const;

	/**
	 * @brief Sets a tile/dPlayer to be occupied by the player
	 * @param position tile to update
	 * @param isMoving specifies whether the player is moving or not (true/moving results in a negative index in dPlayer)
	 */
	void occupyTile(Point position, bool isMoving) const;

	/** @brief Checks if the player level is owned by local client. */
	bool isLevelOwnedByLocalClient() const;

	/** @brief Checks if the player is holding an item of the provided type, and is usable. */
	bool isHoldingItem(const ItemType type) const;

	bool hasNoLife() const;

	bool hasNoMana() const;
};

Player *PlayerAtPosition(Point position, bool ignoreMovingPlayers = false);

void PlrClrTrans(Point position);
void PlrDoTrans(Point position);
void RemovePlrMissiles(const Player &player);
void SetPlayerHitPoints(Player &player, int val);
void InitDungMsgs(Player &player);
void PlayDungMsgs();

} // namespace devilution
