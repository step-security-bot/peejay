//===- include/peejay/small_vector.hpp --------------------*- mode: C++ -*-===//
//*                      _ _                  _              *
//*  ___ _ __ ___   __ _| | | __   _____  ___| |_ ___  _ __  *
//* / __| '_ ` _ \ / _` | | | \ \ / / _ \/ __| __/ _ \| '__| *
//* \__ \ | | | | | (_| | | |  \ V /  __/ (__| || (_) | |    *
//* |___/_| |_| |_|\__,_|_|_|   \_/ \___|\___|\__\___/|_|    *
//*                                                          *
//===----------------------------------------------------------------------===//
//
// Distributed under the Apache License v2.0.
// See https://github.com/paulhuggett/peejay/blob/main/LICENSE.TXT
// for license information.
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//
/// \file small_vector.hpp
/// \brief Provides a small, normally stack allocated, buffer but which can be
///   resized dynamically when necessary.

#ifndef PEEJAY_SMALL_VECTOR_HPP
#define PEEJAY_SMALL_VECTOR_HPP

#include <array>
#include <cstddef>
#include <initializer_list>
#include <new>
#include <variant>
#include <vector>

#include "peejay/arrayvec.hpp"

namespace peejay {

/// A class which provides a vector-like interface to a small, normally stack
/// allocated, buffer which may, if necessary, be resized. It is normally used
/// to contain string buffers where they are typically small enough to be
/// stack-allocated, but where the code must gracefully suport arbitrary
/// lengths.
template <typename ElementType,
          std::size_t BodyElements = 256 / sizeof (ElementType)>
class small_vector {
  template <typename, std::size_t>
  friend class small_vector;

public:
  using value_type = ElementType;

  using reference = value_type &;
  using const_reference = value_type const &;
  using pointer = value_type *;
  using const_pointer = value_type const *;

  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  using iterator = pointer_based_iterator<value_type>;
  using const_iterator = pointer_based_iterator<value_type const>;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  /// Constructs the container with an initial size of 0.
  small_vector () noexcept = default;
  /// Constructs the container with the contents of the range [\p first, \p
  /// last).
  ///
  /// \tparam InputIterator  A type which satisfies std::input_iterator<>.
  /// \param first  The start of the range from which to copy the elements.
  /// \param last  The end of the range from which to copy the elements.
  template <typename InputIterator,
            typename = std::enable_if_t<input_iterator<InputIterator>>>
  small_vector (InputIterator first, InputIterator last);
  /// Constructs the container with the given initial number of elements.
  explicit small_vector (std::size_t required_elements);
  /// Constructs the container with \p count copies of elements with value
  /// \p value.
  ///
  /// \param count  The number of elements to be initialized.
  /// \param value  The value with which to initialize elements of the
  ///   container.
  small_vector (size_type count, const_reference value);
  /// Constructs the container with a given initial collection of values.
  small_vector (std::initializer_list<ElementType> init);

  template <std::size_t OtherSize>
  small_vector (small_vector<ElementType, OtherSize> const &rhs) {
    this->transfer_from (rhs);
  }
  small_vector (small_vector const &rhs) = default;

  template <std::size_t OtherSize>
  small_vector (small_vector<ElementType, OtherSize> &&rhs) noexcept {
    this->transfer_from (std::move (rhs));
  }
  small_vector (small_vector &&rhs) noexcept (
      std::is_nothrow_move_constructible_v<ElementType>) = default;

  ~small_vector () noexcept = default;

  template <std::size_t OtherSize>
  small_vector &operator= (small_vector<ElementType, OtherSize> const &other) {
    this->transfer_from (other);
    return *this;
  }
  small_vector &operator= (small_vector const &other) = default;

  template <std::size_t OtherSize>
  small_vector &operator= (small_vector<ElementType, OtherSize> &&other) {
    this->transfer_from (std::move (other));
    return *this;
  }
  small_vector &operator= (small_vector &&other) noexcept (
      std::is_nothrow_move_constructible_v<ElementType>
          &&std::is_nothrow_move_assignable_v<ElementType>) = default;

  /// \name Element access
  ///@{
  const_pointer data () const noexcept {
    return visit (*this, [] (auto const &a) noexcept { return std::data (a); });
  }
  pointer data () noexcept {
    return visit (*this, [] (auto &a) noexcept { return std::data (a); });
  }

  const_reference operator[] (std::size_t n) const noexcept {
    return visit (*this, [n] (auto const &a) noexcept -> const_reference {
      using asize = typename std::decay_t<decltype (a)>::size_type;
      return a[static_cast<asize> (n)];
    });
  }
  reference operator[] (std::size_t n) noexcept {
    return visit (*this, [n] (auto &a) noexcept -> reference {
      using asize = typename std::decay_t<decltype (a)>::size_type;
      return a[static_cast<asize> (n)];
    });
  }

  const_reference back () const {
    return visit (*this,
                  [] (auto const &a) -> const_reference { return a.back (); });
  }
  reference back () {
    return visit (*this, [] (auto &a) -> reference { return a.back (); });
  }

  ///@}

  /// \name Capacity
  ///@{
  /// Returns the number of elements.
  [[nodiscard]] std::size_t size () const noexcept {
    return visit (*this, [] (auto const &a) noexcept {
      return static_cast<std::size_t> (std::size (a));
    });
  }
  [[nodiscard]] std::size_t size_bytes () const noexcept {
    return this->size () * sizeof (ElementType);
  }

  /// Checks whether the container is empty.
  [[nodiscard]] bool empty () const noexcept { return size () == 0U; }

  /// Returns the number of elements that can be held in currently allocated
  /// storage.
  [[nodiscard]] std::size_t capacity () const noexcept {
    auto const *const large_arr = std::get_if<large_type> (&arr_);
    return std::max (BodyElements, large_arr != nullptr ? large_arr->capacity ()
                                                        : std::size_t{0});
  }

  /// The number of elements stored within the body of the object.
  [[nodiscard]] static constexpr std::size_t body_elements () noexcept {
    return BodyElements;
  }

  /// \brief Increase the capacity of the vector to a value that's greater or
  ///   equal to \p new_cap.
  ///
  /// If \p new_cap is greater than the current capacity(), new storage is
  /// allocated, otherwise the method does nothing. reserve() does not change
  /// the size of the vector.
  ///
  /// \note If \p new_cap is greater than capacity(), all iterators, including
  /// the past-the-end iterator, and all references to the elements are
  /// invalidated. Otherwise, no iterators or references are invalidated.
  ///
  /// \param new_cap  The new capacity of the vector.
  void reserve (std::size_t new_cap);

  /// \brief Resizes the container so that it is large enough for accommodate
  ///   the given number of elements.
  ///
  /// \note Calling this function invalidates the contents of the buffer and
  ///   any iterators.
  ///
  /// \param count  The number of elements that the container is to
  ///   accommodate.
  void resize (std::size_t count);
  /// Resizes the container so that it is large enough for accommodate the
  /// given number of elements.
  ///
  /// \note Calling this function invalidates the contents of the buffer and
  ///   any iterators.
  ///
  /// \param count  The number of elements that the container is to
  ///   accommodate.
  /// \param value  The value with which to initialize the new elements.
  void resize (size_type count, const_reference value);
  ///@}

  /// \name Iterators
  ///@{
  /// Returns an iterator to the beginning of the container.
  constexpr const_iterator begin () const noexcept {
    return const_iterator{data ()};
  }
  constexpr iterator begin () noexcept { return iterator{data ()}; }
  constexpr const_iterator cbegin () noexcept {
    return const_iterator{data ()};
  }
  /// Returns a reverse iterator to the first element of the reversed
  /// container. It corresponds to the last element of the non-reversed
  /// container.
  constexpr reverse_iterator rbegin () noexcept {
    return reverse_iterator{this->end ()};
  }
  constexpr const_reverse_iterator rbegin () const noexcept {
    return const_reverse_iterator{this->end ()};
  }
  constexpr const_reverse_iterator rcbegin () noexcept {
    return const_reverse_iterator{this->cend ()};
  }

  /// Returns an iterator to the end of the container.
  constexpr const_iterator end () const noexcept {
    return const_iterator{data () + size ()};
  }
  constexpr iterator end () noexcept { return iterator{data () + size ()}; }
  constexpr const_iterator cend () noexcept {
    return const_iterator{data () + size ()};
  }
  constexpr reverse_iterator rend () noexcept {
    return reverse_iterator{this->begin ()};
  }
  constexpr const_reverse_iterator rend () const noexcept {
    return const_reverse_iterator{this->begin ()};
  }
  constexpr const_reverse_iterator rcend () noexcept {
    return const_reverse_iterator{this->cbegin ()};
  }
  ///@}

  /// \name Modifiers
  ///@{

  /// Removes all elements from the container.
  /// Invalidates any references, pointers, or iterators referring to contained
  /// elements. Any past-the-end iterators are also invalidated.
  void clear () noexcept {
    visit (*this, [] (auto &a) { a.clear (); });
  }

  /// Erases the specified element from the container. Invalidates iterators
  /// and references at or after the point of the erase, including the end()
  /// iterator.
  ///
  /// \p pos Iterator to the element to remove.
  /// \returns Iterator following the last removed element. If \p pos refers
  ///   to the last element, then the end() iterator is returned.
  iterator erase (const_iterator pos);
  /// Erases the elements in the range [\p first, \p last). Invalidates
  /// iterators and references at or after the point of the erase, including
  /// the end() iterator.
  ///
  /// \p first  The first of the range of elements to remove.
  /// \p last  The last of the range of elements to remove.
  /// \returns Iterator following the last removed element. If last == end()
  ///   prior to removal, then the updated end() iterator is returned. If
  ///   [\p first, \p last) is an empty range, then last is returned.
  iterator erase (const_iterator first, const_iterator last);

  /// Adds an element to the end.
  void push_back (ElementType const &v);
  template <typename... Args>
  void emplace_back (Args &&...args);

  /// Replaces the contents with \p count copies of value \p value.
  ///
  /// \param count The new size of the container.
  /// \param value The value with which to initialize elements of the container.
  void assign (size_type count, const_reference value);
  template <typename InputIt>
  void assign (InputIt first, InputIt last);

  void assign (std::initializer_list<ElementType> ilist) {
    this->assign (std::begin (ilist), std::end (ilist));
  }

  /// Add the specified range to the end of the small vector.
  template <typename InputIt>
  void append (InputIt first, InputIt last);
  void append (std::initializer_list<ElementType> ilist) {
    this->append (std::begin (ilist), std::end (ilist));
  }

  /// \param pos  Iterator before which the new elements will be constructed.
  /// \param count  The number of copies to be inserted.
  /// \param value  Element value to insert.
  /// \returns  An iterator pointing to the first of the new elements or \p pos
  ///   if \p count == 0.
  iterator insert (const_iterator pos, size_type count, const_reference value);

  /// \brief Removes the last element of the container.
  ///
  /// Calling pop_back() on an empty container results in undefined behavior.
  /// Iterators and references to the last element, as well as the end()
  /// iterator, are invalidated.
  void pop_back () {
    visit (*this, [] (auto &v) { v.pop_back (); });
  }
  ///@}

private:
  /// A "small" in-object buffer that is used for relatively small
  /// allocations.
  using small_type = arrayvec<ElementType, BodyElements>;
  /// A (potentially) large buffer that is used to satify requests for
  /// buffer element counts that are too large for type 'small'.
  using large_type = std::vector<ElementType>;
  std::variant<small_type, large_type> arr_;

  template <typename T>
  void transfer_from (T &&rhs);

  template <std::size_t Index, typename OtherVector>
  void transfer_alternative_from (OtherVector &&rhs);

  template <std::size_t Index, std::size_t OtherSize>
  void transfer_from_same (small_vector<ElementType, OtherSize> const &rhs) {
    std::get<Index> (arr_) = std::get<Index> (rhs.arr_);
  }
  template <std::size_t Index, std::size_t OtherSize>
  void
  transfer_from_same (small_vector<ElementType, OtherSize> &&rhs) noexcept (
      std::is_rvalue_reference_v<decltype (rhs)>
          &&std::is_nothrow_move_assignable_v<
              std::variant_alternative_t<Index, decltype (arr_)>>) {
    std::get<Index> (arr_) = std::move (std::get<Index> (rhs.arr_));
  }

  template <std::size_t OtherSize>
  void transfer_from_different (
      small_vector<ElementType, OtherSize> const &rhs) {
    visit (*this,
           [&rhs] (auto &v) { v.assign (std::begin (rhs), std::end (rhs)); });
  }
  template <std::size_t OtherSize>
  void transfer_from_different (small_vector<ElementType, OtherSize> &&rhs) {
    visit (*this, [&rhs] (auto &v) {
      std::move (std::begin (rhs), std::end (rhs), std::back_inserter (v));
    });
  }

  template <typename SmallVector, typename Visitor,
            typename = std::enable_if_t<
                std::is_same_v<SmallVector, small_vector> ||
                std::is_same_v<SmallVector, small_vector const>>>
  static decltype (auto) visit (SmallVector &sv, Visitor visitor) noexcept (
      std::is_nothrow_invocable_v<Visitor, small_type>
          &&std::is_nothrow_invocable_v<Visitor, large_type>) {
    assert (!sv.arr_.valueless_by_exception ());
    if (auto *const small = std::get_if<small_type> (&sv.arr_)) {
      return visitor (*small);
    }
    if (auto *const large = std::get_if<large_type> (&sv.arr_)) {
      return visitor (*large);
    }
    unreachable ();
  }

  large_type &to_large ();
};

// (ctor)
// ~~~~~~
template <typename ElementType, std::size_t BodyElements>
template <typename InputIterator, typename>
small_vector<ElementType, BodyElements>::small_vector (InputIterator first,
                                                       InputIterator last) {
  if constexpr (forward_iterator<InputIterator>) {
    if (auto const count = std::distance (first, last);
        count >= 0 && static_cast<std::make_unsigned_t<decltype (count)>> (
                          count) <= BodyElements) {
      arr_.template emplace<small_type> (first, last);
    } else {
      arr_.template emplace<large_type> (first, last);
    }
    return;
  }
  // A single-pass fallback algorithm for input iterators.
  std::for_each (first, last,
                 [this] (auto const &v) { this->emplace_back (v); });
}

template <typename ElementType, std::size_t BodyElements>
small_vector<ElementType, BodyElements>::small_vector (
    std::size_t const required_elements) {
  if (required_elements <= BodyElements) {
    arr_.template emplace<small_type> (
        static_cast<typename small_type::size_type> (required_elements));
  } else {
    arr_.template emplace<large_type> (required_elements);
  }
}

template <typename ElementType, std::size_t BodyElements>
small_vector<ElementType, BodyElements>::small_vector (size_type count,
                                                       const_reference value) {
  if (count <= BodyElements) {
    arr_.template emplace<small_type> (
        static_cast<typename small_type::size_type> (count), value);
  } else {
    arr_.template emplace<large_type> (count, value);
  }
}

template <typename ElementType, std::size_t BodyElements>
small_vector<ElementType, BodyElements>::small_vector (
    std::initializer_list<ElementType> init)
    : small_vector () {
  if (init.size () <= BodyElements) {
    arr_.template emplace<small_type> (init);
  } else {
    arr_.template emplace<large_type> (init);
  }
}

// to large
// ~~~~~~~~
template <typename ElementType, std::size_t BodyElements>
auto small_vector<ElementType, BodyElements>::to_large () -> large_type & {
  assert (std::holds_alternative<small_type> (arr_));
  if (auto const *const sm = std::get_if<small_type> (&arr_)) {
    // Switch from small to large.
    std::vector<ElementType> vec{std::begin (*sm), std::end (*sm)};
    static_assert (std::is_nothrow_move_constructible_v<large_type>);
    arr_.template emplace<large_type> (std::move (vec));
  }
  assert (std::holds_alternative<large_type> (arr_));
  return *std::get_if<large_type> (&arr_);
}

// reserve
// ~~~~~~~
template <typename ElementType, std::size_t BodyElements>
void small_vector<ElementType, BodyElements>::reserve (
    std::size_t const new_cap) {
  assert (!arr_.valueless_by_exception ());
  if (auto const *const sm = std::get_if<small_type> (&arr_)) {
    if (new_cap <= sm->capacity ()) {
      return;  // Resizing within the capacity of the small buffer.
    }
    this->to_large ();
  }
  assert (std::holds_alternative<large_type> (arr_));
  std::get_if<large_type> (&arr_)->reserve (new_cap);
}

// resize
// ~~~~~~
template <typename ElementType, std::size_t BodyElements>
void small_vector<ElementType, BodyElements>::resize (size_type count) {
  this->resize (count, ElementType{});
}

template <typename ElementType, std::size_t BodyElements>
void small_vector<ElementType, BodyElements>::resize (size_type count,
                                                      const_reference value) {
  assert (!arr_.valueless_by_exception ());
  if (auto *const vec = std::get_if<large_type> (&arr_)) {
    // Resize entirely within the large buffer.
    vec->resize (count, value);
    return;
  }
  if (count <= BodyElements) {
    // Resize entirely within the small buffer.
    assert (std::holds_alternative<small_type> (arr_));
    std::get_if<small_type> (&arr_)->resize (
        static_cast<typename small_type::size_type> (count), value);
  } else {
    this->to_large ().resize (count, value);
  }
}

// push back
// ~~~~~~~~~
template <typename ElementType, std::size_t BodyElements>
inline void small_vector<ElementType, BodyElements>::push_back (
    ElementType const &v) {
  assert (!arr_.valueless_by_exception ());
  if (auto *const vec = std::get_if<large_type> (&arr_)) {
    return vec->push_back (v);
  }
  assert (std::holds_alternative<small_type> (arr_));
  auto *const sm = std::get_if<small_type> (&arr_);
  if (sm->size () < BodyElements) {
    sm->push_back (v);
  } else {
    this->to_large ().push_back (v);
  }
}

// emplace back
// ~~~~~~~~~~~~
template <typename ElementType, std::size_t BodyElements>
template <typename... Args>
inline void small_vector<ElementType, BodyElements>::emplace_back (
    Args &&...args) {
  assert (!arr_.valueless_by_exception ());
  if (auto *const vec = std::get_if<large_type> (&arr_)) {
    return vec->emplace_back (std::forward<Args> (args)...);
  }
  assert (std::holds_alternative<small_type> (arr_));
  auto *const sm = std::get_if<small_type> (&arr_);
  if (sm->size () < BodyElements) {
    sm->emplace_back (std::forward<Args> (args)...);
  } else {
    this->to_large ().emplace_back (std::forward<Args> (args)...);
  }
}

// assign
// ~~~~~~
template <typename ElementType, std::size_t BodyElements>
void small_vector<ElementType, BodyElements>::assign (size_type count,
                                                      const_reference value) {
  if (count <= BodyElements) {
    if (auto *const small = std::get_if<small_type> (&arr_)) {
      small->assign (count, value);
      return;
    }
  }
  if (auto *const large = std::get_if<large_type> (&arr_)) {
    large->assign (count, value);
  } else {
    large_type vec (count, value);
    static_assert (std::is_nothrow_move_constructible_v<large_type>);
    arr_.template emplace<large_type> (std::move (vec));
  }
}

template <typename ElementType, std::size_t BodyElements>
template <typename InputIt>
void small_vector<ElementType, BodyElements>::assign (InputIt first,
                                                      InputIt last) {
  this->clear ();
  this->append (first, last);
}

// append
// ~~~~~~
template <typename ElementType, std::size_t BodyElements>
template <typename Iterator>
void small_vector<ElementType, BodyElements>::append (Iterator first,
                                                      Iterator last) {
  for (; first != last; ++first) {
    this->push_back (*first);
  }
}

// erase
// ~~~~~
template <typename ElementType, std::size_t BodyElements>
auto small_vector<ElementType, BodyElements>::erase (const_iterator pos)
    -> iterator {
  return visit (*this, [this, pos] (auto &v) {
    // Convert 'pos' to an iterator in v.
    auto const vpos = v.begin () + (to_address (pos) - data ());
    // Do the erase itself.
    auto const it = v.erase (vpos);
    // convert the result into an iterator in this.
    return iterator{data () + (it - v.begin ())};
  });
}

template <typename ElementType, std::size_t BodyElements>
auto small_vector<ElementType, BodyElements>::erase (const_iterator first,
                                                     const_iterator last)
    -> iterator {
  return visit (*this, [this, first, last] (auto &v) {
    auto b = v.begin ();
    auto *const d = data ();
    auto const vfirst = b + (to_address (first) - d);
    auto const vlast = b + (to_address (last) - d);

    auto const it = v.erase (vfirst, vlast);
    // convert the result into an iterator in this.
    return iterator{data () + (it - v.begin ())};
  });
}

// insert
// ~~~~~~
template <typename ElementType, std::size_t BodyElements>
auto small_vector<ElementType, BodyElements>::insert (const_iterator pos,
                                                      size_type count,
                                                      const_reference value)
    -> iterator {
  assert (!arr_.valueless_by_exception ());
  // Compute the index _before_ potentially converting the buffer to "large".
  auto const index = to_address (pos) - this->data ();
  if (auto const *const sm = std::get_if<small_type> (&arr_)) {
    if (sm->capacity () + count > BodyElements) {
      this->to_large ();
    }
  }
  return std::visit (
      [this, index, count, &value] (auto &v) {
        using T = std::decay_t<decltype (v)>;
        auto const it =
            v.insert (std::begin (v) + index,
                      static_cast<typename T::size_type> (count), value);
        return iterator{data () + (it - v.begin ())};
      },
      arr_);
}

// transfer from
// ~~~~~~~~~~~~~
template <typename ElementType, std::size_t BodyElements>
template <std::size_t Index, typename OtherVector>
void small_vector<ElementType, BodyElements>::transfer_alternative_from (
    OtherVector &&rhs) {
  // Source and destination are both using the desired alternative, so we can
  // copy/move directly between them.
  if (arr_.index () == Index && rhs.arr_.index () == Index) {
    this->transfer_from_same<Index> (std::forward<OtherVector> (rhs));
  } else {
    // Source and/or destination are not using the desired alternative.
    static_assert (std::is_nothrow_default_constructible_v<
                       std::variant_alternative_t<Index, decltype (arr_)>>,
                   "default ctor must be noexcept so that the variant cannot "
                   "become valueless");
    arr_.template emplace<Index> ();
    this->transfer_from_different (std::forward<OtherVector> (rhs));
  }
}

template <typename ElementType, std::size_t BodyElements>
template <typename OtherVector>
void small_vector<ElementType, BodyElements>::transfer_from (
    OtherVector &&rhs) {
  constexpr auto small_index = std::size_t{0};
  constexpr auto large_index = std::size_t{1};
  static_assert (
      std::is_same_v<small_type, std::variant_alternative_t<small_index,
                                                            decltype (arr_)>> &&
          std::is_same_v<large_type, std::variant_alternative_t<
                                         large_index, decltype (arr_)>>,
      "small_index and/or large_index must be wrong");

  // If the rhs container will fit into our "small" container then that's what
  // we'll produce otherwise we must use the "large" alternative. Note that the
  // 'rhs' container has a different value for BodyElements so it may be using
  // either alternative.
  if (rhs.size () <= BodyElements) {
    this->transfer_alternative_from<small_index> (
        std::forward<OtherVector> (rhs));
  } else {
    this->transfer_alternative_from<large_index> (
        std::forward<OtherVector> (rhs));
  }
}

template <typename ElementType, std::size_t LhsBodyElements,
          std::size_t RhsBodyElements>
bool operator== (small_vector<ElementType, LhsBodyElements> const &lhs,
                 small_vector<ElementType, RhsBodyElements> const &rhs) {
  return std::equal (std::begin (lhs), std::end (lhs), std::begin (rhs),
                     std::end (rhs));
}
template <typename ElementType, std::size_t LhsBodyElements,
          std::size_t RhsBodyElements>
bool operator!= (small_vector<ElementType, LhsBodyElements> const &lhs,
                 small_vector<ElementType, RhsBodyElements> const &rhs) {
  return !operator== (lhs, rhs);
}

}  // end namespace peejay

#endif  // PEEJAY_SMALL_VECTOR_HPP
