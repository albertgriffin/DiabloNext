#include "players/death.hpp"

#include <cstdint>
#include <optional>
#include <utility>

#include <fmt/core.h>

#include "control/control.hpp"
#include "cursor.h"
#include "engine/backbuffer_state.hpp"
#include "engine/path.h"
#include "game_mode.hpp"
#include "gamemenu.h"
#include "inv.h"
#include "players/item_iterators.hpp"
#include "items.h"
#include "levels/gendung.h"
#include "lua/lua_event.hpp"
#include "missiles.h"
#include "monster.h"
#include "msg.h"
#include "player.h"
#include "players/animation.hpp"
#include "players/combat.hpp"
#include "players/movement.hpp"
#include "utils/is_of.hpp"
#include "utils/utf8.hpp"

namespace devilution {
namespace {

void RespawnDeadItem(Item &&itm, Point target)
{
	if (ActiveItemCount >= MAXITEMS)
		return;

	const int ii = AllocateItem();
	Item &item = Items[ii];

	dItem[target.x][target.y] = ii + 1;

	item = itm;
	item.position = target;
	RespawnItem(item, true);
	NetSendCmdPItem(false, CMD_SPAWNITEM, target, item);
}

void DeadItem(Player &player, Item &&item, Displacement direction)
{
	if (item.isEmpty())
		return;

	const Point playerTile = player.position.tile;
	if (direction != Displacement { 0, 0 }) {
		const Point target = playerTile + direction;
		if (ItemSpaceOk(target)) {
			RespawnDeadItem(std::move(item), target);
			return;
		}
	}

	std::optional<Point> dropPoint = FindClosestValidPosition(ItemSpaceOk, playerTile, 1, 50);
	if (dropPoint) {
		RespawnDeadItem(std::move(item), *dropPoint);
	}
}

int DropGold(Player &player, int amount, bool skipFullStacks)
{
	for (int i = 0; i < player._pNumInv && amount > 0; i++) {
		Item &item = player.InvList[i];

		if (item._itype != ItemType::Gold || (skipFullStacks && item._ivalue == MaxGold))
			continue;

		if (amount < item._ivalue) {
			Item goldItem;
			MakeGoldStack(goldItem, amount);
			DeadItem(player, std::move(goldItem), { 0, 0 });

			item._ivalue -= amount;

			return 0;
		}

		amount -= item._ivalue;
		DeadItem(player, std::move(item), { 0, 0 });
		player.RemoveInvItem(i);
		i = -1;
	}

	return amount;
}

void DropHalfPlayersGold(Player &player)
{
	const int remainingGold = DropGold(player, player._pGold / 2, true);
	if (remainingGold > 0) {
		DropGold(player, remainingGold, false);
	}

	player._pGold /= 2;
}

} // namespace

bool DoDeath(Player &player)
{
	if (player.AnimInfo.isLastFrame()) {
		if (player.AnimInfo.tickCounterOfCurrentFrame == 0) {
			player.AnimInfo.ticksPerFrame = 100;
			dFlags[player.position.tile.x][player.position.tile.y] |= DungeonFlag::DeadPlayer;
		} else if (&player == MyPlayer && player.AnimInfo.tickCounterOfCurrentFrame == 30) {
			MyPlayerIsDead = true;
		}
	}

	return false;
}

bool PlrDeathModeOK(Player &player)
{
	if (&player != MyPlayer) {
		return true;
	}
	if (player._pmode == PM_DEATH) {
		return true;
	}
	if (player._pmode == PM_QUIT) {
		return true;
	}
	if (player._pmode == PM_NEWLVL) {
		return true;
	}

	return false;
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((no_sanitize("shift-base")))
#endif
void
StartPlayerKill(Player &player, DeathReason deathReason)
{
	if (player.hasNoLife() && player._pmode == PM_DEATH) {
		return;
	}

	if (&player == MyPlayer) {
		NetSendCmdParam1(true, CMD_PLRDEAD, static_cast<uint16_t>(deathReason));
		gamemenu_off();
	}

	const bool dropGold = !gbIsMultiplayer || !(player.isOnLevel(16) || player.isOnArenaLevel());
	const bool dropItems = dropGold && deathReason == DeathReason::MonsterOrTrap;
	const bool dropEar = dropGold && deathReason == DeathReason::Player;

	player.Say(HeroSpeech::AuughUh);

	// Are the current animations item dependent?
	if (player._pgfxnum != 0) {
		if (dropItems) {
			// Ensure death animation show the player without weapon and armor, because they drop on death
			player._pgfxnum = 0;
		} else {
			// Death animation aren't weapon specific, so always use the unarmed animations
			player._pgfxnum &= ~0xFU;
		}
		ResetPlayerGFX(player);
		SetPlrAnims(player);
	}

	NewPlrAnim(player, player_graphic::Death, player._pdir);

	player._pBlockFlag = false;
	player._pmode = PM_DEATH;
	player._pInvincible = true;
	SetPlayerHitPoints(player, 0);

	if (&player != MyPlayer && dropItems) {
		// Ensure that items are removed for remote players
		// The dropped items will be synced separately (by the remote client)
		for (Item &item : player.InvBody) {
			item.clear();
		}
		CalcPlrInv(player, false);
	}

	if (player.isOnActiveLevel()) {
		FixPlayerLocation(player, player._pdir);
		FixPlrWalkTags(player);
		dFlags[player.position.tile.x][player.position.tile.y] |= DungeonFlag::DeadPlayer;
		SetPlayerOld(player);

		// Only generate drops once (for the local player)
		// For remote players we get separated sync messages (by the remote client)
		if (&player == MyPlayer) {
			RedrawComponent(PanelDrawComponent::Health);

			if (!player.HoldItem.isEmpty()) {
				DeadItem(player, std::move(player.HoldItem), { 0, 0 });
				NewCursor(CURSOR_HAND);
			}
			if (dropGold) {
				DropHalfPlayersGold(player);
			}
			if (dropEar) {
				Item ear;
				InitializeItem(ear, IDI_EAR);
				CopyUtf8(ear._iName, fmt::format(fmt::runtime("Ear of {:s}"), player._pName), sizeof(ear._iName));
				CopyUtf8(ear._iIName, player._pName, ItemNameLength);
				switch (player._pClass) {
				case HeroClass::Sorcerer:
					ear._iCurs = ICURS_EAR_SORCERER;
					break;
				case HeroClass::Warrior:
					ear._iCurs = ICURS_EAR_WARRIOR;
					break;
				case HeroClass::Rogue:
				case HeroClass::Monk:
				case HeroClass::Bard:
				case HeroClass::Barbarian:
					ear._iCurs = ICURS_EAR_ROGUE;
					break;
				default:
					break;
				}

				ear._iCreateInfo = player._pName[0] << 8 | player._pName[1];
				ear._iSeed = player._pName[2] << 24 | player._pName[3] << 16 | player._pName[4] << 8 | player._pName[5];
				ear._ivalue = player.getCharacterLevel();

				if (FindGetItem(ear._iSeed, IDI_EAR, ear._iCreateInfo) == -1) {
					DeadItem(player, std::move(ear), { 0, 0 });
				}
			}
			if (dropItems) {
				Direction pdd = player._pdir;
				for (Item &item : player.InvBody) {
					pdd = Left(pdd);
					DeadItem(player, item.pop(), Displacement(pdd));
				}

				CalcPlrInv(player, false);
			}
		}
	}
	SetPlayerHitPoints(player, 0);
}

void StripTopGold(Player &player)
{
	for (Item &item : InventoryPlayerItemsRange { player }) {
		if (item._itype != ItemType::Gold)
			continue;
		if (item._ivalue <= MaxGold)
			continue;
		Item excessGold;
		MakeGoldStack(excessGold, item._ivalue - MaxGold);
		item._ivalue = MaxGold;

		if (GoldAutoPlace(player, excessGold))
			continue;
		if (!player.HoldItem.isEmpty() && ActiveItemCount + 1 >= MAXITEMS)
			continue;
		DeadItem(player, std::move(excessGold), { 0, 0 });
	}
	player._pGold = CalculateGold(player);

	if (player.HoldItem.isEmpty())
		return;
	if (AutoEquip(player, player.HoldItem, false))
		return;
	if (CanFitItemInInventory(player, player.HoldItem))
		return;
	if (AutoPlaceItemInBelt(player, player.HoldItem))
		return;
	const std::optional<Point> itemTile = FindAdjacentPositionForItem(player.position.tile, player._pdir);
	if (itemTile)
		return;
	DeadItem(player, std::move(player.HoldItem), { 0, 0 });
	NewCursor(CURSOR_HAND);
}

void ApplyPlrDamage(DamageType damageType, Player &player, int dam, int minHP /*= 0*/, int frac /*= 0*/, DeathReason deathReason /*= DeathReason::MonsterOrTrap*/)
{
	int totalDamage = (dam << 6) + frac;
	if (&player == MyPlayer && !player.hasNoLife()) {
		lua::OnPlayerTakeDamage(&player, totalDamage, static_cast<int>(damageType));
	}
	if (totalDamage > 0) {
		RefreshPlayerCombatCooldown(player);
	}
	if (totalDamage > 0 && player.pManaShield && HasNoneOf(player._pIFlags, ItemSpecialEffect::NoMana)) {
		const uint8_t manaShieldLevel = player._pSplLvl[static_cast<int8_t>(SpellID::ManaShield)];
		if (manaShieldLevel > 0) {
			totalDamage += totalDamage / -player.GetManaShieldDamageReduction();
		}
		if (&player == MyPlayer)
			RedrawComponent(PanelDrawComponent::Mana);
		if (player._pMana >= totalDamage) {
			player._pMana -= totalDamage;
			player._pManaBase -= totalDamage;
			totalDamage = 0;
		} else {
			totalDamage -= player._pMana;
			if (manaShieldLevel > 0) {
				totalDamage += totalDamage / (player.GetManaShieldDamageReduction() - 1);
			}
			player._pMana = 0;
			player._pManaBase = player._pMaxManaBase - player._pMaxMana;
			if (&player == MyPlayer)
				NetSendCmd(true, CMD_REMSHIELD);
		}
	}

	if (totalDamage == 0)
		return;

	RedrawComponent(PanelDrawComponent::Health);
	player._pHitPoints -= totalDamage;
	player._pHPBase -= totalDamage;
	if (player._pHitPoints > player._pMaxHP) {
		player._pHitPoints = player._pMaxHP;
		player._pHPBase = player._pMaxHPBase;
	}
	const int minHitPoints = minHP << 6;
	if (player._pHitPoints < minHitPoints) {
		SetPlayerHitPoints(player, minHitPoints);
	}
	if (player.hasNoLife()) {
		SyncPlrKill(player, deathReason);
	}
}

void SyncPlrKill(Player &player, DeathReason deathReason)
{
	SetPlayerHitPoints(player, 0);
	StartPlayerKill(player, deathReason);
}

} // namespace devilution
