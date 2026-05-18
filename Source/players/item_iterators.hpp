#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

#include "inv_iterators.hpp"
#include "utils/algorithm/container.hpp"

namespace devilution {

struct Player;

/**
 * @brief A range over equipped player items.
 */
template <typename PlayerT>
class EquippedPlayerItemsRange {
	static_assert(std::is_same_v<PlayerT, Player> || std::is_same_v<PlayerT, const Player>,
	    "The template argument must be `Player` or `const Player`");
	using ItemT = std::conditional_t<std::is_const_v<PlayerT>, const Item, Item>;
	using Iterator = typename ItemsContainerRange<ItemT>::Iterator;

public:
	explicit EquippedPlayerItemsRange(PlayerT &player);

	[[nodiscard]] Iterator begin() const;
	[[nodiscard]] Iterator end() const;

private:
	[[nodiscard]] std::size_t containerSize() const;

	PlayerT *player_;
};

/**
 * @brief A range over non-equipped inventory player items.
 */
template <typename PlayerT>
class InventoryPlayerItemsRange {
	static_assert(std::is_same_v<PlayerT, Player> || std::is_same_v<PlayerT, const Player>,
	    "The template argument must be `Player` or `const Player`");
	using ItemT = std::conditional_t<std::is_const_v<PlayerT>, const Item, Item>;
	using Iterator = typename ItemsContainerRange<ItemT>::Iterator;

public:
	explicit InventoryPlayerItemsRange(PlayerT &player);

	[[nodiscard]] Iterator begin() const;
	[[nodiscard]] Iterator end() const;

private:
	[[nodiscard]] std::size_t containerSize() const;

	PlayerT *player_;
};

/**
 * @brief A range over belt player items.
 */
template <typename PlayerT>
class BeltPlayerItemsRange {
	static_assert(std::is_same_v<PlayerT, Player> || std::is_same_v<PlayerT, const Player>,
	    "The template argument must be `Player` or `const Player`");
	using ItemT = std::conditional_t<std::is_const_v<PlayerT>, const Item, Item>;
	using Iterator = typename ItemsContainerRange<ItemT>::Iterator;

public:
	explicit BeltPlayerItemsRange(PlayerT &player);

	[[nodiscard]] Iterator begin() const;
	[[nodiscard]] Iterator end() const;

private:
	[[nodiscard]] std::size_t containerSize() const;

	PlayerT *player_;
};

/**
 * @brief A range over non-equipped player items in the following order: Inventory, Belt.
 */
template <typename PlayerT>
class InventoryAndBeltPlayerItemsRange {
	static_assert(std::is_same_v<PlayerT, Player> || std::is_same_v<PlayerT, const Player>,
	    "The template argument must be `Player` or `const Player`");
	using ItemT = std::conditional_t<std::is_const_v<PlayerT>, const Item, Item>;
	using Iterator = typename ItemsContainerListRange<ItemT>::Iterator;

public:
	explicit InventoryAndBeltPlayerItemsRange(PlayerT &player);

	[[nodiscard]] Iterator begin() const;
	[[nodiscard]] Iterator end() const;

private:
	PlayerT *player_;
};

/**
 * @brief A range over non-empty player items in the following order: Equipped, Inventory, Belt.
 */
template <typename PlayerT>
class PlayerItemsRange {
	static_assert(std::is_same_v<PlayerT, Player> || std::is_same_v<PlayerT, const Player>,
	    "The template argument must be `Player` or `const Player`");
	using ItemT = std::conditional_t<std::is_const_v<PlayerT>, const Item, Item>;
	using Iterator = typename ItemsContainerListRange<ItemT>::Iterator;

public:
	explicit PlayerItemsRange(PlayerT &player);

	[[nodiscard]] Iterator begin() const;
	[[nodiscard]] Iterator end() const;

private:
	PlayerT *player_;
};

extern template class EquippedPlayerItemsRange<Player>;
extern template class EquippedPlayerItemsRange<const Player>;
extern template class InventoryPlayerItemsRange<Player>;
extern template class InventoryPlayerItemsRange<const Player>;
extern template class BeltPlayerItemsRange<Player>;
extern template class BeltPlayerItemsRange<const Player>;
extern template class InventoryAndBeltPlayerItemsRange<Player>;
extern template class InventoryAndBeltPlayerItemsRange<const Player>;
extern template class PlayerItemsRange<Player>;
extern template class PlayerItemsRange<const Player>;

void RemoveInventoryItemAt(Player &player, int itemIndex);
void RemoveBeltItemAt(Player &player, int itemIndex);

/**
 * @brief Checks whether the player has an inventory item matching the predicate.
 */
template <typename Predicate>
bool HasInventoryItem(const Player &player, Predicate &&predicate)
{
	const InventoryPlayerItemsRange items { player };
	return c_find_if(items, std::forward<Predicate>(predicate)) != items.end();
}

/**
 * @brief Checks whether the player has a belt item matching the predicate.
 */
template <typename Predicate>
bool HasBeltItem(const Player &player, Predicate &&predicate)
{
	const BeltPlayerItemsRange items { player };
	return c_find_if(items, std::forward<Predicate>(predicate)) != items.end();
}

/**
 * @brief Checks whether the player has an inventory or a belt item matching the predicate.
 */
template <typename Predicate>
bool HasInventoryOrBeltItem(const Player &player, Predicate &&predicate)
{
	return HasInventoryItem(player, predicate) || HasBeltItem(player, predicate);
}

/**
 * @brief Checks whether the player has an inventory item with the given ID (IDidx).
 */
inline bool HasInventoryItemWithId(const Player &player, _item_indexes id)
{
	return HasInventoryItem(player, [id](const Item &item) {
		return item.IDidx == id;
	});
}

/**
 * @brief Checks whether the player has a belt item with the given ID (IDidx).
 */
inline bool HasBeltItemWithId(const Player &player, _item_indexes id)
{
	return HasBeltItem(player, [id](const Item &item) {
		return item.IDidx == id;
	});
}

/**
 * @brief Checks whether the player has an inventory or a belt item with the given ID (IDidx).
 */
inline bool HasInventoryOrBeltItemWithId(const Player &player, _item_indexes id)
{
	return HasInventoryItemWithId(player, id) || HasBeltItemWithId(player, id);
}

/**
 * @brief Removes the first inventory item matching the predicate.
 *
 * @return Whether an item was found and removed.
 */
template <typename Predicate>
bool RemoveInventoryItem(Player &player, Predicate &&predicate)
{
	const InventoryPlayerItemsRange items { player };
	const auto it = c_find_if(items, std::forward<Predicate>(predicate));
	if (it == items.end())
		return false;
	RemoveInventoryItemAt(player, static_cast<int>(it.index()));
	return true;
}

/**
 * @brief Removes the first belt item matching the predicate.
 *
 * @return Whether an item was found and removed.
 */
template <typename Predicate>
bool RemoveBeltItem(Player &player, Predicate &&predicate)
{
	const BeltPlayerItemsRange items { player };
	const auto it = c_find_if(items, std::forward<Predicate>(predicate));
	if (it == items.end())
		return false;
	RemoveBeltItemAt(player, static_cast<int>(it.index()));
	return true;
}

/**
 * @brief Removes the first inventory or belt item matching the predicate.
 *
 * @return Whether an item was found and removed.
 */
template <typename Predicate>
bool RemoveInventoryOrBeltItem(Player &player, Predicate &&predicate)
{
	return RemoveInventoryItem(player, predicate) || RemoveBeltItem(player, predicate);
}

/**
 * @brief Removes the first inventory item with the given id (IDidx).
 *
 * @return Whether an item was found and removed.
 */
inline bool RemoveInventoryItemById(Player &player, _item_indexes id)
{
	return RemoveInventoryItem(player, [id](const Item &item) {
		return item.IDidx == id;
	});
}

/**
 * @brief Removes the first belt item with the given id (IDidx).
 *
 * @return Whether an item was found and removed.
 */
inline bool RemoveBeltItemById(Player &player, _item_indexes id)
{
	return RemoveBeltItem(player, [id](const Item &item) {
		return item.IDidx == id;
	});
}

/**
 * @brief Removes the first inventory or belt item with the given id (IDidx).
 *
 * @return Whether an item was found and removed.
 */
inline bool RemoveInventoryOrBeltItemById(Player &player, _item_indexes id)
{
	return RemoveInventoryItemById(player, id) || RemoveBeltItemById(player, id);
}

} // namespace devilution
