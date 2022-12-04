//===- include/peejay/null.hpp ----------------------------*- mode: C++ -*-===//
//*              _ _  *
//*  _ __  _   _| | | *
//* | '_ \| | | | | | *
//* | | | | |_| | | | *
//* |_| |_|\__,_|_|_| *
//*                   *
//===----------------------------------------------------------------------===//
//
// Distributed under the Apache License v2.0.
// See https://github.com/paulhuggett/peejay/blob/main/LICENSE.TXT
// for license information.
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//

#ifndef PEEJAY_NULL_HPP
#define PEEJAY_NULL_HPP

#include <cstdint>
#include <string_view>
#include <system_error>

#include "peejay/json.hpp"

namespace peejay {

class null {
public:
  static constexpr void result () noexcept {
    // The null output produces no result at all.
  }

  static std::error_code string_value (u8string_view const &) noexcept {
    return {};
  }
  static std::error_code int64_value (std::int64_t) noexcept { return {}; }
  static std::error_code uint64_value (std::uint64_t) noexcept { return {}; }
  static std::error_code double_value (double) noexcept { return {}; }
  static std::error_code boolean_value (bool) noexcept { return {}; }
  static std::error_code null_value () noexcept { return {}; }

  static std::error_code begin_array () noexcept { return {}; }
  static std::error_code end_array () noexcept { return {}; }

  static std::error_code begin_object () noexcept { return {}; }
  static std::error_code key (peejay::u8string_view const &) noexcept {
    return {};
  }
  static std::error_code end_object () noexcept { return {}; }
};

}  // end namespace peejay

#endif  // PEEJAY_NULL_HPP
