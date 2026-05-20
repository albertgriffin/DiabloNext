#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#ifdef USE_SDL3
#include <SDL3/SDL_pixels.h>
#else
#include <SDL.h>
#endif

#include "engine/lighting_defs.hpp"
#include "engine/point.hpp"
#include "engine/render/dun_render.hpp"
#include "engine/surface.hpp"
#include "levels/dun_tile.hpp"
#include "lighting.h"
#include "utils/sdl_compat.h"
#include "utils/sdl_wrap.h"

namespace devilution {
namespace {

class LightTablePointerGuard {
public:
	LightTablePointerGuard(uint8_t *fullyLit, uint8_t *fullyDark)
	    : oldFullyLit_(FullyLitLightTable)
	    , oldFullyDark_(FullyDarkLightTable)
	{
		FullyLitLightTable = fullyLit;
		FullyDarkLightTable = fullyDark;
	}

	~LightTablePointerGuard()
	{
		FullyLitLightTable = oldFullyLit_;
		FullyDarkLightTable = oldFullyDark_;
	}

private:
	uint8_t *oldFullyLit_;
	uint8_t *oldFullyDark_;
};

uint32_t HashSurfacePixels(const Surface &surface)
{
	uint32_t hash = 2166136261U;
	for (int y = 0; y < surface.h(); y++) {
		for (int x = 0; x < surface.w(); x++) {
			hash ^= surface[Point { x, y }];
			hash *= 16777619U;
		}
	}
	return hash;
}

std::array<uint8_t, LightTableSize> IdentityLightTable()
{
	std::array<uint8_t, LightTableSize> table {};
	for (size_t i = 0; i < table.size(); i++) {
		table[i] = static_cast<uint8_t>(i);
	}
	return table;
}

std::array<uint8_t, LightTableSize> HalfLightTable()
{
	std::array<uint8_t, LightTableSize> table {};
	for (size_t i = 0; i < table.size(); i++) {
		table[i] = static_cast<uint8_t>(i / 2);
	}
	return table;
}

std::array<uint8_t, DunFrameWidth * DunFrameHeight> GradientSquareTile()
{
	std::array<uint8_t, DunFrameWidth * DunFrameHeight> tile {};
	for (int y = 0; y < DunFrameHeight; y++) {
		for (int x = 0; x < DunFrameWidth; x++) {
			tile[y * DunFrameWidth + x] = static_cast<uint8_t>(((x * 5) + (y * 7)) % 255);
		}
	}
	return tile;
}

TEST(RenderVisualRegression, ClassicLightTableSceneMatchesApprovedSnapshot)
{
	auto fullyLit = IdentityLightTable();
	auto partiallyLit = HalfLightTable();
	std::array<uint8_t, LightTableSize> fullyDark {};
	const LightTablePointerGuard lightTableGuard { fullyLit.data(), fullyDark.data() };
	const auto tile = GradientSquareTile();

	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 112, 44, 8, SDL_PIXELFORMAT_INDEX8);
	ASSERT_NE(surface, nullptr);
	Surface out { surface.get() };
	ASSERT_TRUE(SDL_FillSurfaceRect(surface.get(), nullptr, 3));

	RenderTileFrame(out, { 4, 37 }, TileType::Square, tile.data(), DunFrameHeight, MaskType::Solid, fullyLit.data());
	RenderTileFrame(out, { 40, 37 }, TileType::Square, tile.data(), DunFrameHeight, MaskType::Solid, partiallyLit.data());
	RenderTileFrame(out, { 76, 37 }, TileType::Square, tile.data(), DunFrameHeight, MaskType::Solid, fullyDark.data());

	EXPECT_EQ(HashSurfacePixels(out), 2123789583U);
	EXPECT_EQ((out[Point { 8, 10 }]), 209U);
	EXPECT_EQ((out[Point { 44, 10 }]), 104U);
	EXPECT_EQ((out[Point { 80, 10 }]), 0U);
	EXPECT_EQ((out[Point { 2, 2 }]), 3U);
}

TEST(RenderVisualRegression, ClassicClippedLightTableSceneMatchesApprovedSnapshot)
{
	auto fullyLit = IdentityLightTable();
	auto partiallyLit = HalfLightTable();
	std::array<uint8_t, LightTableSize> fullyDark {};
	const LightTablePointerGuard lightTableGuard { fullyLit.data(), fullyDark.data() };
	const auto tile = GradientSquareTile();

	SDLSurfaceUniquePtr surface = SDLWrap::CreateRGBSurfaceWithFormat(0, 48, 28, 8, SDL_PIXELFORMAT_INDEX8);
	ASSERT_NE(surface, nullptr);
	Surface out { surface.get() };
	ASSERT_TRUE(SDL_FillSurfaceRect(surface.get(), nullptr, 9));

	RenderTileFrame(out, { -8, 31 }, TileType::Square, tile.data(), DunFrameHeight, MaskType::Solid, fullyLit.data());
	RenderTileFrame(out, { 24, 31 }, TileType::Square, tile.data(), DunFrameHeight, MaskType::Solid, partiallyLit.data());

	EXPECT_EQ(HashSurfacePixels(out), 262715200U);
	EXPECT_EQ((out[Point { 0, 8 }]), 201U);
	EXPECT_EQ((out[Point { 28, 8 }]), 90U);
	EXPECT_EQ((out[Point { 44, 8 }]), 3U);
	EXPECT_EQ((out[Point { 4, 4 }]), 249U);
}

} // namespace
} // namespace devilution
