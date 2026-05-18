#include "players/item_iterators.hpp"

#include "player.h"

namespace devilution {

template <typename PlayerT>
EquippedPlayerItemsRange<PlayerT>::EquippedPlayerItemsRange(PlayerT &player)
    : player_(&player)
{
}

template <typename PlayerT>
typename EquippedPlayerItemsRange<PlayerT>::Iterator EquippedPlayerItemsRange<PlayerT>::begin() const
{
	return Iterator { &player_->InvBody[0], containerSize(), 0 };
}

template <typename PlayerT>
typename EquippedPlayerItemsRange<PlayerT>::Iterator EquippedPlayerItemsRange<PlayerT>::end() const
{
	return Iterator { nullptr, containerSize(), containerSize() };
}

template <typename PlayerT>
std::size_t EquippedPlayerItemsRange<PlayerT>::containerSize() const
{
	return sizeof(player_->InvBody) / sizeof(player_->InvBody[0]);
}

template <typename PlayerT>
InventoryPlayerItemsRange<PlayerT>::InventoryPlayerItemsRange(PlayerT &player)
    : player_(&player)
{
}

template <typename PlayerT>
typename InventoryPlayerItemsRange<PlayerT>::Iterator InventoryPlayerItemsRange<PlayerT>::begin() const
{
	return Iterator { &player_->InvList[0], containerSize(), 0 };
}

template <typename PlayerT>
typename InventoryPlayerItemsRange<PlayerT>::Iterator InventoryPlayerItemsRange<PlayerT>::end() const
{
	return Iterator { nullptr, containerSize(), containerSize() };
}

template <typename PlayerT>
std::size_t InventoryPlayerItemsRange<PlayerT>::containerSize() const
{
	return static_cast<std::size_t>(player_->_pNumInv);
}

template <typename PlayerT>
BeltPlayerItemsRange<PlayerT>::BeltPlayerItemsRange(PlayerT &player)
    : player_(&player)
{
}

template <typename PlayerT>
typename BeltPlayerItemsRange<PlayerT>::Iterator BeltPlayerItemsRange<PlayerT>::begin() const
{
	return Iterator { &player_->SpdList[0], containerSize(), 0 };
}

template <typename PlayerT>
typename BeltPlayerItemsRange<PlayerT>::Iterator BeltPlayerItemsRange<PlayerT>::end() const
{
	return Iterator { nullptr, containerSize(), containerSize() };
}

template <typename PlayerT>
std::size_t BeltPlayerItemsRange<PlayerT>::containerSize() const
{
	return sizeof(player_->SpdList) / sizeof(player_->SpdList[0]);
}

template <typename PlayerT>
InventoryAndBeltPlayerItemsRange<PlayerT>::InventoryAndBeltPlayerItemsRange(PlayerT &player)
    : player_(&player)
{
}

template <typename PlayerT>
typename InventoryAndBeltPlayerItemsRange<PlayerT>::Iterator InventoryAndBeltPlayerItemsRange<PlayerT>::begin() const
{
	return Iterator({
	    InventoryPlayerItemsRange(*player_).begin(),
	    BeltPlayerItemsRange(*player_).begin(),
	});
}

template <typename PlayerT>
typename InventoryAndBeltPlayerItemsRange<PlayerT>::Iterator InventoryAndBeltPlayerItemsRange<PlayerT>::end() const
{
	return Iterator({
	    InventoryPlayerItemsRange(*player_).end(),
	    BeltPlayerItemsRange(*player_).end(),
	});
}

template <typename PlayerT>
PlayerItemsRange<PlayerT>::PlayerItemsRange(PlayerT &player)
    : player_(&player)
{
}

template <typename PlayerT>
typename PlayerItemsRange<PlayerT>::Iterator PlayerItemsRange<PlayerT>::begin() const
{
	return Iterator({
	    EquippedPlayerItemsRange(*player_).begin(),
	    InventoryPlayerItemsRange(*player_).begin(),
	    BeltPlayerItemsRange(*player_).begin(),
	});
}

template <typename PlayerT>
typename PlayerItemsRange<PlayerT>::Iterator PlayerItemsRange<PlayerT>::end() const
{
	return Iterator({
	    EquippedPlayerItemsRange(*player_).end(),
	    InventoryPlayerItemsRange(*player_).end(),
	    BeltPlayerItemsRange(*player_).end(),
	});
}

void RemoveInventoryItemAt(Player &player, int itemIndex)
{
	player.RemoveInvItem(itemIndex);
}

void RemoveBeltItemAt(Player &player, int itemIndex)
{
	player.RemoveSpdBarItem(itemIndex);
}

template class EquippedPlayerItemsRange<Player>;
template class EquippedPlayerItemsRange<const Player>;
template class InventoryPlayerItemsRange<Player>;
template class InventoryPlayerItemsRange<const Player>;
template class BeltPlayerItemsRange<Player>;
template class BeltPlayerItemsRange<const Player>;
template class InventoryAndBeltPlayerItemsRange<Player>;
template class InventoryAndBeltPlayerItemsRange<const Player>;
template class PlayerItemsRange<Player>;
template class PlayerItemsRange<const Player>;

} // namespace devilution
