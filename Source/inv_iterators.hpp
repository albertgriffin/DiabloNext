#pragma once

#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>

#include "items.h"

namespace devilution {

/**
 * @brief A range over non-empty items in a container.
 */
template <typename ItemT>
class ItemsContainerRange {
	static_assert(std::is_same_v<ItemT, Item> || std::is_same_v<ItemT, const Item>,
	    "The template argument must be `Item` or `const Item`");

public:
	class Iterator {
	public:
		using iterator_category = std::forward_iterator_tag;
		using difference_type = int;
		using value_type = ItemT;
		using pointer = value_type *;
		using reference = value_type &;

		Iterator() = default;

		Iterator(ItemT *items, std::size_t count, std::size_t index)
		    : items_(items)
		    , count_(count)
		    , index_(index)
		{
			advancePastEmpty();
		}

		pointer operator->() const
		{
			return &items_[index_];
		}

		reference operator*() const
		{
			return items_[index_];
		}

		Iterator &operator++()
		{
			++index_;
			advancePastEmpty();
			return *this;
		}

		Iterator operator++(int)
		{
			auto copy = *this;
			++(*this);
			return copy;
		}

		bool operator==(const Iterator &other) const
		{
			return index_ == other.index_;
		}

		bool operator!=(const Iterator &other) const
		{
			return !(*this == other);
		}

		[[nodiscard]] bool atEnd() const
		{
			return index_ == count_;
		}

		[[nodiscard]] std::size_t index() const
		{
			return index_;
		}

	private:
		void advancePastEmpty()
		{
			while (index_ < count_ && items_[index_].isEmpty()) {
				++index_;
			}
		}

		ItemT *items_ = nullptr;
		std::size_t count_ = 0;
		std::size_t index_ = 0;
	};

	ItemsContainerRange(ItemT *items, std::size_t count)
	    : items_(items)
	    , count_(count)
	{
	}

	[[nodiscard]] Iterator begin() const
	{
		return Iterator { items_, count_, 0 };
	}

	[[nodiscard]] Iterator end() const
	{
		return Iterator { nullptr, count_, count_ };
	}

private:
	ItemT *items_;
	std::size_t count_;
};

/**
 * @brief A range over non-empty items in a list of containers.
 */
template <typename ItemT>
class ItemsContainerListRange {
	static_assert(std::is_same_v<ItemT, Item> || std::is_same_v<ItemT, const Item>,
	    "The template argument must be `Item` or `const Item`");

public:
	class Iterator {
	public:
		using iterator_category = std::forward_iterator_tag;
		using difference_type = int;
		using value_type = ItemT;
		using pointer = value_type *;
		using reference = value_type &;

		Iterator() = default;

		explicit Iterator(std::vector<typename ItemsContainerRange<ItemT>::Iterator> iterators)
		    : iterators_(std::move(iterators))
		{
			advancePastEmpty();
		}

		pointer operator->() const
		{
			return iterators_[current_].operator->();
		}

		reference operator*() const
		{
			return iterators_[current_].operator*();
		}

		Iterator &operator++()
		{
			++iterators_[current_];
			advancePastEmpty();
			return *this;
		}

		Iterator operator++(int)
		{
			auto copy = *this;
			++(*this);
			return copy;
		}

		bool operator==(const Iterator &other) const
		{
			return current_ == other.current_ && iterators_[current_] == other.iterators_[current_];
		}
		bool operator!=(const Iterator &other) const
		{
			return !(*this == other);
		}

	private:
		void advancePastEmpty()
		{
			while (current_ + 1 < iterators_.size() && iterators_[current_].atEnd()) {
				++current_;
			}
		}

		std::vector<typename ItemsContainerRange<ItemT>::Iterator> iterators_;
		std::size_t current_ = 0;
	};
};

} // namespace devilution
