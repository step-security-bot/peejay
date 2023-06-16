#ifndef AV_MEMBER_HPP
#define AV_MEMBER_HPP

#include <cassert>
#include <cstddef>
#include <stdexcept>

class member {
public:
  static inline std::size_t throw_number;

  member () {
    throw_check ();
    ++instances_;
  }
  explicit member (int v) : v_{v} {
    // The memory underlying arrayvec<> is initialized to 0xFF therefore we can
    // check that we're not using uninitialized stored by checking that the
    // values stored by instances of 'member' are not less than 0.
    assert (v >= 0);
    throw_check ();
    ++instances_;
  }
  member (member const& rhs) : v_{rhs.v_} {
    assert (rhs.v_ >= 0);
    throw_check ();
    ++instances_;
  }
  member (member&& rhs) noexcept : v_{rhs.v_} {
    assert (rhs.v_ >= 0);
    ++instances_;
    rhs.v_ = 0;
  }

  ~member () noexcept {
    assert (v_ >= 0);
    --instances_;
  }

  member& operator= (member const& rhs) {
    assert (v_ >= 0 && rhs.v_ >= 0);
    if (&rhs != this) {
      v_ = rhs.v_;
      throw_check ();
    }
    return *this;
  }
  member& operator= (member&& rhs) noexcept {
    assert (v_ >= 0 && rhs.v_ >= 0);
    if (&rhs != this) {
      v_ = rhs.v_;
      rhs.v_ = 0;
    }
    return *this;
  }
  bool operator== (member const& rhs) const noexcept { return v_ == rhs.v_; }
  bool operator!= (member const& rhs) const noexcept { return v_ != rhs.v_; }

  static std::size_t instances () noexcept { return instances_; }

private:
  static inline std::size_t instances_ = 0;
  static inline std::size_t operations_ = 0;

  int v_ = 0;

  static void throw_check () {
    if (operations_ >= throw_number) {
      throw std::runtime_error{"error"};
    }
    ++operations_;
  }
};

#endif  // AV_MEMBER_HPP
