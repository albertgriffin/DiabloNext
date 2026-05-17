#include "player.h"

#include <algorithm>
#include <optional>

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#else
#include <SDL.h>
#endif

#include "control/control.hpp"
#include "controls/control_mode.hpp"
#include "dead.h"
#include "engine/path.h"
#include "engine/random.hpp"
#include "headless_mode.hpp"
#include "interfac.h"
#include "items.h"
#include "levels/gendung.h"
#include "levels/trigs.h"
#include "lighting.h"
#include "loadsave.h"
#include "minitext.h"
#include "monster.h"
#include "msg.h"
#include "multi.h"
#include "nthread.h"
#include "portal.h"
#include "players/animation.hpp"
#include "players/movement.hpp"
#include "qol/stash.h"
#include "utils/is_of.hpp"
#include "utils/log.hpp"

namespace devilution {
namespace {

void InitLevelChange(Player &player)
{
	const Player &myPlayer = *MyPlayer;

	RemoveEnemyReferences(player);
	RemovePlrMissiles(player);
	player.pManaShield = false;
	player.wReflections = 0;
	if (&player != MyPlayer) {
		// share info about your manashield when another player joins the level
		if (myPlayer.pManaShield)
			NetSendCmd(true, CMD_SETSHIELD);
		// share info about your reflect charges when another player joins the level
		NetSendCmdParam1(true, CMD_SETREFLECT, myPlayer.wReflections);
	} else if (qtextflag) {
		qtextflag = false;
		stream_stop();
	}

	FixPlrWalkTags(player);
	SetPlayerOld(player);
	if (&player == MyPlayer) {
		player.occupyTile(player.position.tile, false);
	} else {
		player._pLvlVisited[player.plrlevel] = true;
	}

	ClrPlrPath(player);
	player.destAction = ACTION_NONE;
	player._pLvlChanging = true;

	if (&player == MyPlayer) {
		player.pLvlLoad = 10;
	}
}

} // namespace

/**
 * @param player The player reference.
 * @param c The hero class.
 */
void CreatePlayer(Player &player, HeroClass c)
{
	player = {};
	SetRndSeed(SDL_GetTicks());

	player.setCharacterLevel(1);
	player._pClass = c;

	const ClassAttributes &attr = player.getClassAttributes();

	player._pBaseStr = attr.baseStr;
	player._pStrength = player._pBaseStr;

	player._pBaseMag = attr.baseMag;
	player._pMagic = player._pBaseMag;

	player._pBaseDex = attr.baseDex;
	player._pDexterity = player._pBaseDex;

	player._pBaseVit = attr.baseVit;
	player._pVitality = player._pBaseVit;

	player._pHitPoints = player.calculateBaseLife();
	player._pMaxHP = player._pHitPoints;
	player._pHPBase = player._pHitPoints;
	player._pMaxHPBase = player._pHitPoints;

	player._pMana = player.calculateBaseMana();
	player._pMaxMana = player._pMana;
	player._pManaBase = player._pMana;
	player._pMaxManaBase = player._pMana;

	player._pExperience = 0;
	player._pArmorClass = 0;
	player._pLightRad = 10;
	player._pInfraFlag = false;

	for (uint8_t &spellLevel : player._pSplLvl) {
		spellLevel = 0;
	}

	player._pSpellFlags = SpellFlag::None;
	player._pRSplType = SpellType::Invalid;

	// Initializing the hotkey bindings to no selection
	std::fill(player._pSplHotKey, player._pSplHotKey + NumHotkeys, SpellID::Invalid);

	// CreatePlrItems calls AutoEquip which will overwrite the player graphic if required
	player._pgfxnum = static_cast<uint8_t>(PlayerWeaponGraphic::Unarmed);

	for (bool &levelVisited : player._pLvlVisited) {
		levelVisited = false;
	}

	for (int i = 0; i < 10; i++) {
		player._pSLvlVisited[i] = false;
	}

	player._pLvlChanging = false;
	player.pTownWarps = 0;
	player.pLvlLoad = 0;
	player.pManaShield = false;
	player.pDamAcFlags = ItemSpecialEffectHf::None;
	player.wReflections = 0;

	InitDungMsgs(player);
	CreatePlrItems(player);
	SetRndSeed(0);
}

void InitPlayer(Player &player, bool firstTime)
{
	if (firstTime) {
		player._pRSplType = SpellType::Invalid;
		player._pRSpell = SpellID::Invalid;
		if (&player == MyPlayer)
			LoadHotkeys();
		player._pSBkSpell = SpellID::Invalid;
		player.queuedSpell.spellId = player._pRSpell;
		player.queuedSpell.spellType = player._pRSplType;
		player.pManaShield = false;
		player.wReflections = 0;
	}

	player.lightId = NO_LIGHT;

	if (player.isOnActiveLevel()) {

		SetPlrAnims(player);

		ClearStateVariables(player);

		if (!player.hasNoLife()) {
			player._pmode = PM_STAND;
			NewPlrAnim(player, player_graphic::Stand, Direction::South);
			player.AnimInfo.currentFrame = GenerateRnd(player._pNFrames - 1);
			player.AnimInfo.tickCounterOfCurrentFrame = GenerateRnd(3);
		} else {
			player._pgfxnum &= ~0xFU;
			player._pmode = PM_DEATH;
			NewPlrAnim(player, player_graphic::Death, Direction::South);
			player.AnimInfo.currentFrame = player.AnimInfo.numberOfFrames - 2;
		}

		player._pdir = Direction::South;

		if (&player == MyPlayer && (!firstTime || leveltype != DTYPE_TOWN)) {
			player.position.tile = ViewPosition;
		}

		SetPlayerOld(player);
		player.walkpath[0] = WALK_NONE;
		player.destAction = ACTION_NONE;

		if (&player == MyPlayer) {
			player.lightId = AddLight(player.position.tile, player._pLightRad);
			ChangeLightXY(player.lightId, player.position.tile); // fix for a bug where old light is still visible at the entrance after reentering level
		}
		ActivateVision(player.position.tile, player._pLightRad, player.getId());
	}

	player._pAblSpells = GetSpellBitmask(GetPlayerStartingLoadoutForClass(player._pClass).skill);

	player._pInvincible = false;

	if (&player == MyPlayer) {
		MyPlayerIsDead = false;
	}
}

void InitMultiView()
{
	assert(MyPlayer != nullptr);
	ViewPosition = MyPlayer->position.tile;
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((no_sanitize("shift-base")))
#endif
void
StartNewLvl(Player &player, interface_mode fom, int lvl)
{
	InitLevelChange(player);
	player.outOfCombatSpeedCooldownTicks = 0;

	switch (fom) {
	case WM_DIABNEXTLVL:
	case WM_DIABPREVLVL:
	case WM_DIABRTNLVL:
	case WM_DIABTOWNWARP:
		player.setLevel(lvl);
		break;
	case WM_DIABSETLVL:
		if (&player == MyPlayer)
			setlvlnum = (_setlevels)lvl;
		player.setLevel(setlvlnum);
		break;
	case WM_DIABTWARPUP:
		MyPlayer->pTownWarps |= 1 << (leveltype - 2);
		player.setLevel(lvl);
		break;
	case WM_DIABRETOWN:
		break;
	default:
		app_fatal("StartNewLvl");
	}

	if (&player == MyPlayer) {
		player._pmode = PM_NEWLVL;
		player._pInvincible = true;
		SDL_Event event;
		CustomEventToSdlEvent(event, fom);
		SDL_PushEvent(&event);
		if (gbIsMultiplayer) {
			NetSendCmdParam2(true, CMD_NEWLVL, fom, lvl);
		}
	}
}

void RestartTownLvl(Player &player)
{
	InitLevelChange(player);
	player.outOfCombatSpeedCooldownTicks = 0;

	player.setLevel(0);
	player._pInvincible = false;

	SetPlayerHitPoints(player, 64);

	player._pMana = 0;
	player._pManaBase = player._pMana - (player._pMaxMana - player._pMaxManaBase);

	CalcPlrInv(player, false);
	player._pmode = PM_NEWLVL;

	if (&player == MyPlayer) {
		player._pInvincible = true;
		SDL_Event event;
		CustomEventToSdlEvent(event, WM_DIABRETOWN);
		SDL_PushEvent(&event);
	}
}

void StartWarpLvl(Player &player, size_t pidx)
{
	InitLevelChange(player);

	if (gbIsMultiplayer) {
		if (!player.isOnLevel(0)) {
			player.setLevel(0);
		} else {
			if (Portals[pidx].setlvl)
				player.setLevel(static_cast<_setlevels>(Portals[pidx].level));
			else
				player.setLevel(Portals[pidx].level);
		}
	}

	if (&player == MyPlayer) {
		SetCurrentPortal(pidx);
		player._pmode = PM_NEWLVL;
		player._pInvincible = true;
		SDL_Event event;
		CustomEventToSdlEvent(event, WM_DIABWARPLVL);
		SDL_PushEvent(&event);
	}
}

void SyncPlrAnim(Player &player)
{
	const player_graphic graphic = player.getGraphic();
	if (!HeadlessMode)
		player.AnimInfo.sprites = player.AnimationData[static_cast<size_t>(graphic)].spritesForDirection(player._pdir);
}

void SyncInitPlrPos(Player &player)
{
	if (!player.isOnActiveLevel())
		return;

	const WorldTileDisplacement offset[9] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 0 }, { 0, 2 }, { 1, 2 }, { 2, 1 }, { 2, 2 } };

	const Point position = [&]() {
		for (int i = 0; i < 8; i++) {
			Point position = player.position.tile + offset[i];
			if (PosOkPlayer(player, position))
				return position;
		}

		const std::optional<Point> nearPosition = FindClosestValidPosition(
		    [&player](Point testPosition) {
			    for (int i = 0; i < numtrigs; i++) {
				    if (trigs[i].position == testPosition)
					    return false;
			    }
			    return PosOkPlayer(player, testPosition) && !PosOkPortal(currlevel, testPosition);
		    },
		    player.position.tile,
		    1, // skip the starting tile since that was checked in the previous loop
		    50);

		return nearPosition.value_or(Point { 0, 0 });
	}();

	player.position.tile = position;
	player.occupyTile(position, false);
	player.position.future = position;

	if (&player == MyPlayer) {
		ViewPosition = position;
	}
}

void SyncInitPlr(Player &player)
{
	SetPlrAnims(player);
	SyncInitPlrPos(player);
	if (&player != MyPlayer)
		player.lightId = NO_LIGHT;
}

} // namespace devilution
