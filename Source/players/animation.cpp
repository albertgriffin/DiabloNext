#include "players/animation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "engine/assets.hpp"
#include "engine/load_cl2.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/trn.hpp"
#include "game_mode.hpp"
#include "headless_mode.hpp"
#include "levels/gendung.h"
#include "tables/playerdat.hpp"
#include "tables/spelldat.h"
#include "utils/enum_traits.h"
#include "utils/is_of.hpp"
#include "utils/log.hpp"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

HeroClass GetPlayerSpriteClass(HeroClass cls)
{
	if (cls == HeroClass::Bard && !HaveBardAssets())
		return HeroClass::Rogue;
	if (cls == HeroClass::Barbarian && !HaveBarbarianAssets())
		return HeroClass::Warrior;
	return cls;
}

PlayerWeaponGraphic GetPlayerWeaponGraphic(player_graphic graphic, PlayerWeaponGraphic weaponGraphic)
{
	if (leveltype == DTYPE_TOWN && IsAnyOf(graphic, player_graphic::Lightning, player_graphic::Fire, player_graphic::Magic)) {
		// If the hero doesn't hold the weapon in town then we should use the unarmed animation for casting
		switch (weaponGraphic) {
		case PlayerWeaponGraphic::Mace:
		case PlayerWeaponGraphic::Sword:
			return PlayerWeaponGraphic::Unarmed;
		case PlayerWeaponGraphic::SwordShield:
		case PlayerWeaponGraphic::MaceShield:
			return PlayerWeaponGraphic::UnarmedShield;
		default:
			break;
		}
	}
	return weaponGraphic;
}

uint16_t GetPlayerSpriteWidth(HeroClass cls, player_graphic graphic, PlayerWeaponGraphic weaponGraphic)
{
	const PlayerSpriteData spriteData = GetPlayerSpriteDataForClass(cls);

	switch (graphic) {
	case player_graphic::Stand:
		return spriteData.stand;
	case player_graphic::Walk:
		return spriteData.walk;
	case player_graphic::Attack:
		if (weaponGraphic == PlayerWeaponGraphic::Bow)
			return spriteData.bow;
		return spriteData.attack;
	case player_graphic::Hit:
		return spriteData.swHit;
	case player_graphic::Block:
		return spriteData.block;
	case player_graphic::Lightning:
		return spriteData.lightning;
	case player_graphic::Fire:
		return spriteData.fire;
	case player_graphic::Magic:
		return spriteData.magic;
	case player_graphic::Death:
		return spriteData.death;
	}
	app_fatal("Invalid player_graphic");
}

void GetPlayerGraphicsPath(std::string_view path, std::string_view prefix, std::string_view type, char out[256])
{
	*BufCopy(out, "plrgfx\\", path, "\\", prefix, "\\", prefix, type) = '\0';
}

} // namespace

player_graphic GetPlayerGraphicForSpell(SpellID spellId)
{
	switch (GetSpellData(spellId).type()) {
	case MagicType::Fire:
		return player_graphic::Fire;
	case MagicType::Lightning:
		return player_graphic::Lightning;
	default:
		return player_graphic::Magic;
	}
}

player_graphic Player::getGraphic() const
{
	switch (_pmode) {
	case PM_STAND:
	case PM_NEWLVL:
	case PM_QUIT:
		return player_graphic::Stand;
	case PM_WALK_NORTHWARDS:
	case PM_WALK_SOUTHWARDS:
	case PM_WALK_SIDEWAYS:
		return player_graphic::Walk;
	case PM_ATTACK:
	case PM_RATTACK:
		return player_graphic::Attack;
	case PM_BLOCK:
		return player_graphic::Block;
	case PM_SPELL:
		return GetPlayerGraphicForSpell(executedSpell.spellId);
	case PM_GOTHIT:
		return player_graphic::Hit;
	case PM_DEATH:
		return player_graphic::Death;
	default:
		app_fatal("SyncPlrAnim");
	}
}

uint16_t Player::getSpriteWidth() const
{
	if (!HeadlessMode)
		return (*AnimInfo.sprites)[0].width();
	const player_graphic graphic = getGraphic();
	const HeroClass cls = GetPlayerSpriteClass(_pClass);
	const PlayerWeaponGraphic weaponGraphic = GetPlayerWeaponGraphic(graphic, static_cast<PlayerWeaponGraphic>(_pgfxnum & 0xF));
	return GetPlayerSpriteWidth(cls, graphic, weaponGraphic);
}

void Player::getAnimationFramesAndTicksPerFrame(player_graphic graphics, int8_t &numberOfFrames, int8_t &ticksPerFrame) const
{
	ticksPerFrame = 1;
	switch (graphics) {
	case player_graphic::Stand:
		numberOfFrames = _pNFrames;
		ticksPerFrame = 4;
		break;
	case player_graphic::Walk:
		numberOfFrames = _pWFrames;
		break;
	case player_graphic::Attack:
		numberOfFrames = _pAFrames;
		break;
	case player_graphic::Hit:
		numberOfFrames = _pHFrames;
		break;
	case player_graphic::Lightning:
	case player_graphic::Fire:
	case player_graphic::Magic:
		numberOfFrames = _pSFrames;
		break;
	case player_graphic::Death:
		numberOfFrames = _pDFrames;
		ticksPerFrame = 2;
		break;
	case player_graphic::Block:
		numberOfFrames = _pBFrames;
		ticksPerFrame = 3;
		break;
	default:
		app_fatal("Unknown player graphics");
	}
}

ClxSprite GetPlayerPortraitSprite(Player &player)
{
	const bool inDungeon = (player.plrlevel != 0);

	const HeroClass cls = GetPlayerSpriteClass(player._pClass);
	const PlayerWeaponGraphic animWeaponId = GetPlayerWeaponGraphic(player_graphic::Stand, static_cast<PlayerWeaponGraphic>(player._pgfxnum & 0xF));

	const PlayerSpriteData &spriteData = GetPlayerSpriteDataForClass(cls);
	const char *path = spriteData.classPath.c_str();

	std::string_view szCel = inDungeon ? "as" : "st";

	player_graphic graphic = player_graphic::Stand;
	if (player.hasNoLife()) {
		if (animWeaponId == PlayerWeaponGraphic::Unarmed) {
			szCel = "dt";
			graphic = player_graphic::Death;
		}
	}

	const char prefixBuf[3] = { spriteData.classChar, ArmourChar[player._pgfxnum >> 4], WepChar[static_cast<std::size_t>(animWeaponId)] };
	char pszName[256];
	GetPlayerGraphicsPath(path, std::string_view(prefixBuf, 3), szCel, pszName);

	const std::string spritePath { pszName };
	// Check to see if the sprite has updated.
	if (player.PartyInfoSpriteLocations[inDungeon] != spritePath) {
		// The sprite has changed so store the new location
		player.PartyInfoSpriteLocations[inDungeon] = spritePath;

		player.PartyInfoSprites[inDungeon] = std::nullopt;

		// And now load the new sprite and store it
		const uint16_t animationWidth = GetPlayerSpriteWidth(cls, graphic, animWeaponId);
		player.PartyInfoSprites[inDungeon] = LoadCl2Sheet(pszName, animationWidth);
	}

	const ClxSpriteList spriteList = (*player.PartyInfoSprites[inDungeon])[static_cast<size_t>(Direction::South)];
	return spriteList[(graphic == player_graphic::Stand) ? 0 : spriteList.numSprites() - 1];
}

bool IsPlayerUnarmed(Player &player)
{
	const PlayerWeaponGraphic animWeaponId = GetPlayerWeaponGraphic(player_graphic::Stand, static_cast<PlayerWeaponGraphic>(player._pgfxnum & 0xF));
	return animWeaponId == PlayerWeaponGraphic::Unarmed;
}

void LoadPlrGFX(Player &player, player_graphic graphic)
{
	if (HeadlessMode)
		return;

	auto &animationData = player.AnimationData[static_cast<size_t>(graphic)];
	if (animationData.sprites)
		return;

	const HeroClass cls = GetPlayerSpriteClass(player._pClass);
	PlayerWeaponGraphic animWeaponId = GetPlayerWeaponGraphic(graphic, static_cast<PlayerWeaponGraphic>(player._pgfxnum & 0xF));

	const PlayerSpriteData &spriteData = GetPlayerSpriteDataForClass(cls);
	const char *path = spriteData.classPath.c_str();

	std::string_view szCel;
	switch (graphic) {
	case player_graphic::Stand:
		szCel = "as";
		if (leveltype == DTYPE_TOWN)
			szCel = "st";
		break;
	case player_graphic::Walk:
		szCel = "aw";
		if (leveltype == DTYPE_TOWN)
			szCel = "wl";
		break;
	case player_graphic::Attack:
		if (leveltype == DTYPE_TOWN)
			return;
		szCel = "at";
		break;
	case player_graphic::Hit:
		if (leveltype == DTYPE_TOWN)
			return;
		szCel = "ht";
		break;
	case player_graphic::Lightning:
		szCel = "lm";
		break;
	case player_graphic::Fire:
		szCel = "fm";
		break;
	case player_graphic::Magic:
		szCel = "qm";
		break;
	case player_graphic::Death:
		// Only one Death animation exists, for unarmed characters
		animWeaponId = PlayerWeaponGraphic::Unarmed;
		szCel = "dt";
		break;
	case player_graphic::Block:
		if (leveltype == DTYPE_TOWN)
			return;
		if (!player._pBlockFlag)
			return;
		szCel = "bl";
		break;
	default:
		app_fatal("PLR:2");
	}

	const char prefixBuf[3] = { spriteData.classChar, ArmourChar[player._pgfxnum >> 4], WepChar[static_cast<std::size_t>(animWeaponId)] };
	char pszName[256];
	GetPlayerGraphicsPath(path, std::string_view(prefixBuf, 3), szCel, pszName);
	const uint16_t animationWidth = GetPlayerSpriteWidth(cls, graphic, animWeaponId);
	animationData.sprites = LoadCl2Sheet(pszName, animationWidth);
	std::optional<std::array<uint8_t, 256>> graphicTRN = GetPlayerGraphicTRN(pszName);
	if (graphicTRN) {
		ClxApplyTrans(*animationData.sprites, graphicTRN->data());
	}
	std::optional<std::array<uint8_t, 256>> classTRN = GetClassTRN(player);
	if (classTRN) {
		ClxApplyTrans(*animationData.sprites, classTRN->data());
	}
}

void InitPlayerGFX(Player &player)
{
	if (HeadlessMode)
		return;

	ResetPlayerGFX(player);

	if (player.hasNoLife()) {
		player._pgfxnum &= ~0xFU;
		LoadPlrGFX(player, player_graphic::Death);
		return;
	}

	for (size_t i = 0; i < enum_size<player_graphic>::value; i++) {
		auto graphic = static_cast<player_graphic>(i);
		if (graphic == player_graphic::Death)
			continue;
		LoadPlrGFX(player, graphic);
	}
}

void ResetPlayerGFX(Player &player)
{
	player.AnimInfo.sprites = std::nullopt;

	if (!gbRunGame) {
		player.PartyInfoSprites[0] = std::nullopt;
		player.PartyInfoSprites[1] = std::nullopt;
	}

	for (PlayerAnimationData &animData : player.AnimationData) {
		animData.sprites = std::nullopt;
	}
}

void NewPlrAnim(Player &player, player_graphic graphic, Direction dir, AnimationDistributionFlags flags /*= AnimationDistributionFlags::None*/, int8_t numSkippedFrames /*= 0*/, int8_t distributeFramesBeforeFrame /*= 0*/)
{
	LoadPlrGFX(player, graphic);

	OptionalClxSpriteList sprites;
	int previewShownGameTickFragments = 0;
	if (!HeadlessMode) {
		sprites = player.AnimationData[static_cast<size_t>(graphic)].spritesForDirection(dir);
		if (player.previewCelSprite && (*sprites)[0] == *player.previewCelSprite && !player.isWalking()) {
			previewShownGameTickFragments = std::clamp<int>(AnimationInfo::baseValueFraction - player.progressToNextGameTickWhenPreviewWasSet, 0, AnimationInfo::baseValueFraction);
		}
	}

	int8_t numberOfFrames;
	int8_t ticksPerFrame;
	player.getAnimationFramesAndTicksPerFrame(graphic, numberOfFrames, ticksPerFrame);
	player.AnimInfo.setNewAnimation(sprites, numberOfFrames, ticksPerFrame, flags, numSkippedFrames, distributeFramesBeforeFrame, static_cast<uint8_t>(previewShownGameTickFragments));
}

void SetPlrAnims(Player &player)
{
	const HeroClass pc = player._pClass;
	const PlayerAnimData &plrAtkAnimData = GetPlayerAnimDataForClass(pc);
	auto gn = static_cast<PlayerWeaponGraphic>(player._pgfxnum & 0xFU);

	if (leveltype == DTYPE_TOWN) {
		player._pNFrames = plrAtkAnimData.townIdleFrames;
		player._pWFrames = plrAtkAnimData.townWalkingFrames;
	} else {
		player._pNFrames = plrAtkAnimData.idleFrames;
		player._pWFrames = plrAtkAnimData.walkingFrames;
		player._pHFrames = plrAtkAnimData.recoveryFrames;
		player._pBFrames = plrAtkAnimData.blockingFrames;
		switch (gn) {
		case PlayerWeaponGraphic::Unarmed:
			player._pAFrames = plrAtkAnimData.unarmedFrames;
			player._pAFNum = plrAtkAnimData.unarmedActionFrame;
			break;
		case PlayerWeaponGraphic::UnarmedShield:
			player._pAFrames = plrAtkAnimData.unarmedShieldFrames;
			player._pAFNum = plrAtkAnimData.unarmedShieldActionFrame;
			break;
		case PlayerWeaponGraphic::Sword:
			player._pAFrames = plrAtkAnimData.swordFrames;
			player._pAFNum = plrAtkAnimData.swordActionFrame;
			break;
		case PlayerWeaponGraphic::SwordShield:
			player._pAFrames = plrAtkAnimData.swordShieldFrames;
			player._pAFNum = plrAtkAnimData.swordShieldActionFrame;
			break;
		case PlayerWeaponGraphic::Bow:
			player._pAFrames = plrAtkAnimData.bowFrames;
			player._pAFNum = plrAtkAnimData.bowActionFrame;
			break;
		case PlayerWeaponGraphic::Axe:
			player._pAFrames = plrAtkAnimData.axeFrames;
			player._pAFNum = plrAtkAnimData.axeActionFrame;
			break;
		case PlayerWeaponGraphic::Mace:
			player._pAFrames = plrAtkAnimData.maceFrames;
			player._pAFNum = plrAtkAnimData.maceActionFrame;
			break;
		case PlayerWeaponGraphic::MaceShield:
			player._pAFrames = plrAtkAnimData.maceShieldFrames;
			player._pAFNum = plrAtkAnimData.maceShieldActionFrame;
			break;
		case PlayerWeaponGraphic::Staff:
			player._pAFrames = plrAtkAnimData.staffFrames;
			player._pAFNum = plrAtkAnimData.staffActionFrame;
			break;
		}
	}

	player._pDFrames = plrAtkAnimData.deathFrames;
	player._pSFrames = plrAtkAnimData.castingFrames;
	player._pSFNum = plrAtkAnimData.castingActionFrame;
	const int armorGraphicIndex = player._pgfxnum & ~0xFU;
	if (IsAnyOf(pc, HeroClass::Warrior, HeroClass::Barbarian)) {
		if (gn == PlayerWeaponGraphic::Bow && leveltype != DTYPE_TOWN)
			player._pNFrames = 8;
		if (armorGraphicIndex > 0)
			player._pDFrames = 15;
	}
}

} // namespace devilution
