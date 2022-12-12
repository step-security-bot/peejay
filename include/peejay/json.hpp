//===- include/peejay/json.hpp ----------------------------*- mode: C++ -*-===//
//*    _                  *
//*   (_)___  ___  _ __   *
//*   | / __|/ _ \| '_ \  *
//*   | \__ \ (_) | | | | *
//*  _/ |___/\___/|_| |_| *
//* |__/                  *
//===----------------------------------------------------------------------===//
//
// Distributed under the Apache License v2.0.
// See https://github.com/paulhuggett/peejay/blob/main/LICENSE.TXT
// for license information.
// SPDX-License-Identifier: Apache-2.0
//
//===----------------------------------------------------------------------===//
#ifndef PEEJAY_JSON_HPP
#define PEEJAY_JSON_HPP

#include "peejay/json_error.hpp"
#include "peejay/portab.hpp"

#define ICUBABY_INSIDE_NS peejay
#include "peejay/icubaby.hpp"
#undef ICUBABY_INSIDE_NS

// Standard library
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <ostream>
#include <stack>
#include <string>
#include <tuple>
#include <variant>

// C++20 standard library
#if PEEJAY_CXX20
#include <concepts>
#include <span>
#endif

namespace peejay {

using char8 = icubaby::char8;
using u8string = icubaby::u8string;
using u8string_view = icubaby::u8string_view;

/// A type that is always false. Used to improve the failure mesages from
/// static_assert().
template <typename... T>
[[maybe_unused]] constexpr bool always_false = false;

[[noreturn, maybe_unused]] inline void unreachable () {
  // Uses compiler specific extensions if possible.
  // Even if no extension is used, undefined behavior is still raised by
  // an empty function body and the noreturn attribute.
#ifdef __GNUC__  // GCC, Clang, ICC
  __builtin_unreachable ();
#elif defined(_MSC_VER)  // MSVC
  __assume (false);
#endif
}

#if PEEJAY_CXX20
template <typename T>
concept backend = requires (T &&v) {
  /// Returns the result of the parse. If the parse was successful, this
  /// function is called by parser<>::eof() which will return its result.
  {v.result ()};

  /// Called when a JSON string has been parsed.
  { v.string_value (u8string_view{}) } -> std::convertible_to<std::error_code>;
  /// Called when an integer value has been parsed.
  { v.int64_value (std::int64_t{}) } -> std::convertible_to<std::error_code>;
  /// Called when an unsigned integer value has been parsed.
  { v.uint64_value (std::uint64_t{}) } -> std::convertible_to<std::error_code>;
  /// Called when a floating-point value has been parsed.
  { v.double_value (double{}) } -> std::convertible_to<std::error_code>;
  /// Called when a boolean value has been parsed
  { v.boolean_value (bool{}) } -> std::convertible_to<std::error_code>;
  /// Called when a null value has been parsed.
  { v.null_value () } -> std::convertible_to<std::error_code>;
  /// Called to notify the start of an array. Subsequent event notifications are
  /// for members of this array until a matching call to end_array().
  { v.begin_array () } -> std::convertible_to<std::error_code>;
  /// Called indicate that an array has been completely parsed. This will always
  /// follow an earlier call to begin_array().
  { v.end_array () } -> std::convertible_to<std::error_code>;
  /// Called to notify the start of an object. Subsequent event notifications
  /// are for members of this object until a matching call to end_object().
  { v.begin_object () } -> std::convertible_to<std::error_code>;
  /// Called when an object key string has been parsed.
  { v.key (u8string_view{}) } -> std::convertible_to<std::error_code>;
  /// Called to indicate that an object has been completely parsed. This will
  /// always follow an earlier call to begin_object().
  { v.end_object () } -> std::convertible_to<std::error_code>;
};
#endif  // PEEJAY_CXX20

/// \brief JSON parser implementation details.
namespace details {

template <typename Backend>
PEEJAY_CXX20REQUIRES (backend<Backend>)
class matcher;

template <typename Backend>
class false_token_matcher;
template <typename Backend>
class null_token_matcher;
template <typename Backend>
class number_matcher;
template <typename Backend>
class root_matcher;
template <typename Backend>
class string_matcher;
template <typename Backend>
class true_token_matcher;
template <typename Backend>
class whitespace_matcher;

template <typename Backend>
struct singleton_storage;

/// deleter is intended for use as a unique_ptr<> Deleter. It enables
/// unique_ptr<> to be used with a mixture of heap-allocated and
/// placement-new-allocated objects.
template <typename T>
class deleter {
public:
  enum class mode : char { do_delete, do_dtor, do_nothing };

  /// \param m One of the three modes: delete the object, call the destructor, or do nothing.
  constexpr explicit deleter (mode const m) noexcept : mode_{m} {}
  void operator() (T *const p) const noexcept {
    switch (mode_) {
    case mode::do_delete: delete p; break;
    case mode::do_dtor:
      if (p != nullptr) {
        p->~T ();
      }
      break;
    case mode::do_nothing: break;
    }
  }

private:
  mode mode_;
};

}  // end namespace details

struct line {
  explicit constexpr operator unsigned () const noexcept { return x; }
  unsigned x;
};
struct column {
  explicit constexpr operator unsigned () const noexcept { return y; }
  unsigned y;
};

struct coord {
  constexpr coord () noexcept = default;
  constexpr coord (struct column x, struct line y) noexcept
      : line{y}, column{x} {}
  constexpr coord (struct line y, struct column x) noexcept
      : line{y}, column{x} {}

#if PEEJAY_CXX20
  // https://github.com/llvm/llvm-project/issues/55919
  _Pragma ("GCC diagnostic push")
  _Pragma ("GCC diagnostic ignored \"-Wzero-as-null-pointer-constant\"")
  constexpr auto operator<=> (coord const &) const noexcept = default;
  _Pragma ("GCC diagnostic pop")
#else
  constexpr bool operator== (coord const &rhs) const noexcept {
    return std::make_pair (line, column) ==
           std::make_pair (rhs.line, rhs.column);
  }
  constexpr bool operator!= (coord const &rhs) const noexcept {
    return !operator== (rhs);
  }
  constexpr bool operator<(coord const &rhs) const noexcept {
    return std::make_pair (line, column) <
           std::make_pair (rhs.line, rhs.column);
  }
  constexpr bool operator<= (coord const &rhs) const noexcept {
    return std::make_pair (line, column) <=
           std::make_pair (rhs.line, rhs.column);
  }
  constexpr bool operator> (coord const &rhs) const noexcept {
    return std::make_pair (line, column) >
           std::make_pair (rhs.line, rhs.column);
  }
  constexpr bool operator>= (coord const &rhs) const noexcept {
    return std::make_pair (line, column) >=
           std::make_pair (rhs.line, rhs.column);
  }
#endif  // PEEJAY_CXX20

  unsigned line = 1U;
  unsigned column = 1U;
};

inline std::ostream &operator<< (std::ostream &os, coord const &c) {
  return os << c.line << ':' << c.column;
}

enum class extensions : unsigned {
  none = 0U,
  bash_comments = 1U << 0U,
  single_line_comments = 1U << 1U,
  multi_line_comments = 1U << 2U,
  array_trailing_comma = 1U << 3U,
  object_trailing_comma = 1U << 4U,
  single_quote_string = 1U << 5U,
  leading_plus = 1U << 6U,
  all = ~none,
};

constexpr extensions operator| (extensions a, extensions b) noexcept {
  using ut = std::underlying_type_t<extensions>;
  static_assert (std::is_unsigned_v<ut>,
                 "The extensions type must be unsigned");
  return static_cast<extensions> (static_cast<ut> (a) | static_cast<ut> (b));
}

//-MARK:parser
/// \tparam Backend A type meeting the notifications<> requirements.
template <typename Backend>
PEEJAY_CXX20REQUIRES (backend<Backend>)
class parser {
  friend class details::matcher<Backend>;
  friend class details::root_matcher<Backend>;
  friend class details::whitespace_matcher<Backend>;

public:
  explicit parser (extensions extensions = extensions::none)
      : parser (Backend{}, extensions) {}
  template <typename OtherBackend>
  PEEJAY_CXX20REQUIRES (backend<OtherBackend>)
  explicit parser (OtherBackend &&backend,
                   extensions extensions = extensions::none);
  parser (parser const &) = delete;
  parser (parser &&) noexcept (std::is_nothrow_constructible_v<Backend>) =
      default;

  ~parser () noexcept = default;

  parser &operator= (parser const &) = delete;
  parser &operator= (parser &&) noexcept (
      std::is_nothrow_move_assignable_v<Backend>) = default;

  ///@{
  /// Parses a chunk of JSON input. This function may be called repeatedly with
  /// portions of the source data (for example, as the data is received from an
  /// external source). Once all of the data has been received, call the
  /// parser::eof() method.

  /// \param src The data to be parsed.
  parser &input (u8string const &src) {
    return this->input (std::begin (src), std::end (src));
  }
  parser &input (u8string_view const &src) {
    return this->input (std::begin (src), std::end (src));
  }
#if PEEJAY_CXX20
  /// \param span The span of UTF-8 code units to be parsed.
  template <size_t Extent>
  parser &input (std::span<char8_t, Extent> const &span) {
    return this->input (std::begin (span), std::end (span));
  }
  /// \param span The span of UTF-8 code units to be parsed.
  template <size_t Extent>
  parser &input (std::span<char8_t const, Extent> const &span) {
    return this->input (std::begin (span), std::end (span));
  }
#endif  // PEEJAY_CXX20
  /// \param first The element in the half-open range of UTF-8 code-units to be parsed.
  /// \param last The end of the range of UTF-8 code-units to be parsed.
  template <typename InputIterator>
  PEEJAY_CXX20REQUIRES (
      (std::input_iterator<InputIterator> &&
       std::is_same_v<std::decay_t<typename std::iterator_traits<
                          InputIterator>::value_type>,
                      char8_t>))
  parser &input (InputIterator first, InputIterator last);
  ///@}

  /// Informs the parser that the complete input stream has been passed by calls
  /// to parser<>::input().
  ///
  /// \returns If the parse completes successfully, Backend::result()
  /// is called and its result returned.
  decltype (auto) eof ();

  ///@{

  /// \returns True if the parser has signalled an error.
  [[nodiscard]] constexpr bool has_error () const noexcept {
    return static_cast<bool> (error_);
  }
  /// \returns The error code held by the parser.
  [[nodiscard]] constexpr std::error_code const &last_error () const noexcept {
    return error_;
  }

  ///@{
  [[nodiscard]] constexpr Backend &backend () noexcept { return backend_; }
  [[nodiscard]] constexpr Backend const &backend () const noexcept {
    return backend_;
  }
  ///@}

  /// \param flag  A selection of bits from the parser_extensions enum.
  /// \returns True if any of the extensions given by \p flag are enabled by the parser.
  [[nodiscard]] constexpr bool extension_enabled (
      extensions const flag) const noexcept {
    using ut = std::underlying_type_t<extensions>;
    return (static_cast<ut> (extensions_) & static_cast<ut> (flag)) != 0U;
  }

  /// Returns the parser's position in the input text.
  [[nodiscard]] constexpr coord input_pos () const noexcept { return pos_; }
  /// Returns the position of the most recent token in the input text.
  [[nodiscard]] constexpr coord pos () const noexcept { return matcher_pos_; }

private:
  using matcher = details::matcher<Backend>;
  using pointer = std::unique_ptr<matcher, details::deleter<matcher>>;

  static constexpr auto null_pointer () {
    using deleter = typename pointer::deleter_type;
    return pointer{nullptr, deleter{deleter::mode::do_nothing}};
  }

  void consume_code_point (char32_t code_point);

  ///@{
  /// \brief Managing the column and row number (the "coordinate").

  /// Increments the column number.
  void advance_column () noexcept { ++pos_.column; }

  /// Increments the row number and resets the column.
  void advance_row () noexcept {
    // The column number is set to 0. This is because the outer parse loop
    // automatically advances the column number for each character consumed.
    // This happens after the row is advanced by a matcher's consume() function.
    pos_.column = 0U;
    ++pos_.line;
  }

  /// Resets the column count but does not affect the row number.
  void reset_column () noexcept { pos_.column = 0U; }
  ///@}

  /// Records an error for this parse. The parse will stop as soon as a non-zero
  /// error code is recorded. An error may be reported at any time during the
  /// parse; all subsequent text is ignored.
  ///
  /// \param err  The json error code to be stored in the parser.
  bool set_error (std::error_code const &err) noexcept {
    assert (!error_ || err);
    error_ = err;
    return this->has_error ();
  }
  ///@}

  pointer make_root_matcher (bool object_key = false);
  pointer make_whitespace_matcher ();

  template <typename Matcher, typename... Args>
  PEEJAY_CXX20REQUIRES ((std::derived_from<Matcher, matcher>))
  pointer make_terminal_matcher (Args &&...args) {
    Matcher &m = singletons_.terminals_.template emplace<Matcher> (
        std::forward<Args> (args)...);
    using deleter = typename pointer::deleter_type;
    return pointer{&m, deleter{deleter::mode::do_nothing}};
  }

  /// Preallocated storage for "terminal" matchers. These are the matchers,
  /// such as numbers or strings which can't have child objects.
  details::singleton_storage<Backend> singletons_;

  /// The maximum depth to which we allow the parse stack to grow. This value
  /// should be sufficient for any reasonable input: its intention is to prevent
  /// bogus (attack) inputs from causing the parser's memory consumption to grow
  /// uncontrollably.
  static constexpr std::size_t max_stack_depth_ = 200;

  icubaby::t8_32 utf_;
  /// The parse stack.
  std::stack<pointer> stack_;
  std::error_code error_;

  /// Each instance of the string matcher uses this string object to record its
  /// output. This avoids having to create a new instance each time we scan a
  /// string.
  u8string string_;

  /// The column and row number of the parse within the input stream.
  coord pos_;
  coord matcher_pos_;
  extensions extensions_;
  [[no_unique_address]] Backend backend_;
};

template <typename Backend>
parser (Backend) -> parser<Backend>;

template <typename Backend>
PEEJAY_CXX20REQUIRES (backend<std::remove_reference_t<Backend>>)
inline parser<std::remove_reference_t<Backend>> make_parser (
    Backend &&backend, extensions const extensions = extensions::none) {
  return parser<std::remove_reference_t<Backend>>{
      std::forward<Backend> (backend), extensions};
}

// U+0009  Horizontal tab
// U+000A  Line feed
// U+000B  Vertical tab
// U+000C  Form feed
// U+000D  Carriage return
// U+0020  Space
// U+00A0  No-Break Space
// U+1680  Ogham Space Mark
// U+2000  En Quad
// U+2001  Em Quad
// U+2002  En Space
// U+2003  Em Space
// U+2004  Three-Per-Em Space
// U+2005  Four-Per-Em Space
// U+2006  Six-Per-Em Space
// U+2007  Figure Space
// U+2008  Punctuation Space
// U+2009  Thin Space
// U+200A  Hair Space
// U+2028  Line separator
// U+2029  Paragraph separator
// U+202F  Narrow No-Break Space
// U+205F  Medium Mathematical Space
// U+3000  Ideographic Space
// U+FEFF  Byte order mark

enum char_set : char32_t {
  asterisk = char32_t{0x2a},                  // '*'
  backspace = char32_t{0x0008},               // '\b'
  carriage_return = char32_t{0x000d},         // '\r'
  character_tabulation = char32_t{0x0009},    // '\t'
  digit_nine = char32_t{0x0039},              // '9'
  digit_zero = char32_t{0x0030},              // '0'
  form_feed = char32_t{0x000c},               // '\f'
  latin_capital_letter_a = char32_t{0x0041},  // 'A'
  latin_capital_letter_z = char32_t{0x005a},  // 'Z'
  latin_small_letter_a = char32_t{0x0061},    // 'a'
  latin_small_letter_b = char32_t{0x0062},    // 'b'
  latin_small_letter_f = char32_t{0x0066},    // 'f'
  latin_small_letter_n = char32_t{0x006e},    // 'n'
  latin_small_letter_r = char32_t{0x0072},    // 'r'
  latin_small_letter_t = char32_t{0x0074},    // 't'
  latin_small_letter_u = char32_t{0x0075},    // 'u'
  latin_small_letter_z = char32_t{0x007a},    // 'z'
  line_feed = char32_t{0x000a},               // '\n'
  number_sign = char32_t{0x0023},             // '#'
  quotation_mark = char32_t{0x0022},          // '"'
  reverse_solidus = char32_t{0x005c},         // '\'
  solidus = char32_t{0x002f},                 // '/'
  space = char32_t{0x0020},                   // ' '
};

namespace details {

constexpr bool isspace (char32_t const c) noexcept {
  return c == char_set::character_tabulation || c == char_set::line_feed ||
         c == char_set::carriage_return || c == char_set::space;
}
/// Checks if the given character is an alphanumeric character.
constexpr bool isalnum (char32_t const c) noexcept {
  return (c >= digit_zero && c <= digit_nine) ||
         (c >= latin_capital_letter_a && c <= latin_capital_letter_z) ||
         (c >= latin_small_letter_a && c <= latin_small_letter_z);
}

/// The base class for the various state machines ("matchers") which implement
/// the various productions of the JSON grammar.
//-MARK:matcher
template <typename Backend>
PEEJAY_CXX20REQUIRES (backend<Backend>)
class matcher {
public:
  using pointer = std::unique_ptr<matcher, deleter<matcher>>;

  matcher (matcher const &) = delete;
  virtual ~matcher () noexcept = default;
  matcher &operator= (matcher const &) = delete;

  /// Called for each character as it is consumed from the input.
  ///
  /// \param parser The owning parser instance.
  /// \param ch If true, the character to be consumed. An empty value value indicates
  ///   end-of-file.
  /// \returns A pair consisting of a matcher pointer and a boolean. If non-null, the
  ///   matcher is pushed onto the parse stack; if null the same matcher object is
  ///   used to process the next character. The boolean value is false if the same
  ///   character must be passed to the next consume() call; true indicates that
  ///   the character was correctly matched by this consume() call.
  virtual std::pair<pointer, bool> consume (parser<Backend> &parser,
                                            std::optional<char32_t> ch) = 0;

  /// \returns True if this matcher has completed (and reached it's "done" state). The
  /// parser will pop this instance from the parse stack before continuing.
  [[nodiscard]] bool is_done () const noexcept { return state_ == done; }

protected:
  explicit constexpr matcher (int const initial_state) noexcept
      : state_{initial_state} {}

  matcher (matcher &&) noexcept = default;
  matcher &operator= (matcher &&) noexcept = default;

  [[nodiscard]] constexpr int get_state () const noexcept { return state_; }
  void set_state (int const s) noexcept { state_ = s; }

  ///@{
  /// \brief Errors

  /// \returns True if the parser is in an error state.
  bool set_error (parser<Backend> &parser,
                  std::error_code const &err) noexcept {
    bool const has_error = parser.set_error (err);
    if (has_error) {
      set_state (done);
    }
    return has_error;
  }
  ///@}

  pointer make_root_matcher (parser<Backend> &parser, bool object_key = false) {
    return parser.make_root_matcher (object_key);
  }
  pointer make_whitespace_matcher (parser<Backend> &parser) {
    return parser.make_whitespace_matcher ();
  }

  template <typename Matcher, typename... Args>
  pointer make_terminal_matcher (parser<Backend> &parser, Args &&...args) {
    return parser.template make_terminal_matcher<Matcher, Args...> (
        std::forward<Args> (args)...);
  }

  static constexpr auto null_pointer () {
    return parser<Backend>::null_pointer ();
  }

  /// The value to be used for the "done" state in the each of the matcher state
  /// machines.
  static constexpr auto done = 1;

private:
  int state_;
};

//*  _       _             *
//* | |_ ___| |_____ _ _   *
//* |  _/ _ \ / / -_) ' \  *
//*  \__\___/_\_\___|_||_| *
//*                        *
/// A matcher which checks for a specific keyword such as "true", "false", or
/// "null".
/// \tparam Backend  The parser callback structure.
//-MARK:token matcher
template <typename Backend, typename DoneFunction>
PEEJAY_CXX20REQUIRES ((backend<Backend> &&
                       std::invocable<DoneFunction, parser<Backend> &>))
class token_matcher : public matcher<Backend> {
public:
  /// \param text  The string to be matched.
  /// \param done  The function called when the source string is matched.
  explicit token_matcher (char8 const *text, DoneFunction done) noexcept
      : matcher<Backend> (start_state), text_{text}, done_{done} {}
  token_matcher (token_matcher const &) = delete;
  token_matcher (token_matcher &&) noexcept = default;

  ~token_matcher () noexcept override = default;

  token_matcher &operator= (token_matcher const &) = delete;
  token_matcher &operator= (token_matcher &&) noexcept = default;

  std::pair<typename matcher<Backend>::pointer, bool> consume (
      parser<Backend> &parser, std::optional<char32_t> ch) override;

private:
  enum state {
    done_state = matcher<Backend>::done,
    start_state,
    last_state,
  };

  /// The keyword to be matched. The input sequence must exactly match this
  /// string or an unrecognized token error is raised. Once all of the
  /// characters are matched, complete() is called.
  char8 const *text_;

  /// This function is called once the complete token text has been matched.
  [[no_unique_address]] DoneFunction const done_;
};

template <typename Backend, typename DoneFunction>
PEEJAY_CXX20REQUIRES ((backend<Backend> &&
                       std::invocable<DoneFunction, parser<Backend> &>))
std::pair<typename matcher<Backend>::pointer, bool> token_matcher<
    Backend, DoneFunction>::consume (parser<Backend> &parser,
                                     std::optional<char32_t> ch) {
  bool match = true;
  switch (this->get_state ()) {
  case start_state:
    assert (!ch || icubaby::is_code_point_start (*ch));
    if (!ch || *ch != static_cast<char32_t> (*text_)) {
      this->set_error (parser, error::unrecognized_token);
    } else {
      ++text_;
      if (*text_ == '\0') {
        // We've run out of input text, so ensure that the next character isn't
        // alpha-numeric.
        this->set_state (last_state);
      }
    }
    break;
  case last_state:
    if (ch) {
      if (isalnum (*ch) != 0) {
        this->set_error (parser, error::unrecognized_token);
        return {matcher<Backend>::null_pointer (), true};
      }
      match = false;
    }
    this->set_error (parser, done_ (parser));
    this->set_state (done_state);
    break;
  default: unreachable (); break;
  }
  return {matcher<Backend>::null_pointer (), match};
}

//*   __      _           _       _             *
//*  / _|__ _| |___ ___  | |_ ___| |_____ _ _   *
//* |  _/ _` | (_-</ -_) |  _/ _ \ / / -_) ' \  *
//* |_| \__,_|_/__/\___|  \__\___/_\_\___|_||_| *
//*                                             *

//-MARK:false token
template <typename Backend>
struct false_complete {
  std::error_code operator() (parser<Backend> &p) const {
    return p.backend ().boolean_value (false);
  }
};

template <typename Backend>
class false_token_matcher
    : public token_matcher<Backend, false_complete<Backend>> {
public:
  false_token_matcher ()
      : token_matcher<Backend, false_complete<Backend>> (u8"false", {}) {}
};

//*  _                  _       _             *
//* | |_ _ _ _  _ ___  | |_ ___| |_____ _ _   *
//* |  _| '_| || / -_) |  _/ _ \ / / -_) ' \  *
//*  \__|_|  \_,_\___|  \__\___/_\_\___|_||_| *
//*                                           *

//-MARK:true token
template <typename Backend>
struct true_complete {
  std::error_code operator() (parser<Backend> &p) const {
    return p.backend ().boolean_value (true);
  }
};

template <typename Backend>
class true_token_matcher
    : public token_matcher<Backend, true_complete<Backend>> {
public:
  true_token_matcher ()
      : token_matcher<Backend, true_complete<Backend>> (u8"true", {}) {}
};

//*           _ _   _       _             *
//*  _ _ _  _| | | | |_ ___| |_____ _ _   *
//* | ' \ || | | | |  _/ _ \ / / -_) ' \  *
//* |_||_\_,_|_|_|  \__\___/_\_\___|_||_| *
//*                                       *

//-MARK:null token
template <typename Backend>
struct null_complete {
  std::error_code operator() (parser<Backend> &p) const {
    return p.backend ().null_value ();
  }
};

template <typename Backend>
class null_token_matcher
    : public token_matcher<Backend, null_complete<Backend>> {
public:
  null_token_matcher ()
      : token_matcher<Backend, null_complete<Backend>> (u8"null", {}) {}
};

//*                 _              *
//*  _ _ _  _ _ __ | |__  ___ _ _  *
//* | ' \ || | '  \| '_ \/ -_) '_| *
//* |_||_\_,_|_|_|_|_.__/\___|_|   *
//*                                *
// Grammar (from RFC 7159, March 2014)
//     number = [ minus ] int [ frac ] [ exp ]
//     decimal-point = %x2E       ; .
//     digit1-9 = %x31-39         ; 1-9
//     e = %x65 / %x45            ; e E
//     exp = e [ minus / plus ] 1*DIGIT
//     frac = decimal-point 1*DIGIT
//     int = zero / ( digit1-9 *DIGIT )
//     minus = %x2D               ; -
//     plus = %x2B                ; +
//     zero = %x30                ; 0
//-MARK:number
template <typename Backend>
class number_matcher final : public matcher<Backend> {
public:
  number_matcher () noexcept : matcher<Backend> (leading_minus_state) {}
  number_matcher (number_matcher const &) = delete;
  number_matcher (number_matcher &&) noexcept = default;

  ~number_matcher () noexcept override = default;

  number_matcher &operator= (number_matcher const &) = delete;
  number_matcher &operator= (number_matcher &&) noexcept = default;

  std::pair<typename matcher<Backend>::pointer, bool> consume (
      parser<Backend> &parser, std::optional<char32_t> ch) override;

private:
  [[nodiscard]] bool in_terminal_state () const;

  bool do_leading_minus_state (parser<Backend> &parser, char32_t c);
  /// Implements the first character of the 'int' production.
  bool do_integer_initial_digit_state (parser<Backend> &parser, char32_t c);
  bool do_integer_digit_state (parser<Backend> &parser, char32_t c);
  bool do_frac_state (parser<Backend> &parser, char32_t c);
  bool do_frac_digit_state (parser<Backend> &parser, char32_t c);
  bool do_exponent_sign_state (parser<Backend> &parser, char32_t c);
  bool do_exponent_digit_state (parser<Backend> &parser, char32_t c);

  void complete (parser<Backend> &parser);
  void number_is_float ();

  void make_result (parser<Backend> &parser);

  enum state {
    done_state = matcher<Backend>::done,
    leading_minus_state,
    integer_initial_digit_state,
    integer_digit_state,
    frac_state,
    frac_initial_digit_state,
    frac_digit_state,
    exponent_sign_state,
    exponent_initial_digit_state,
    exponent_digit_state,
  };

  bool is_neg_ = false;
  bool is_integer_ = true;
  std::uint64_t int_acc_ = 0;

  struct {
    double frac_part = 0.0;
    double frac_scale = 1.0;
    double whole_part = 0.0;

    bool exp_is_negative = false;
    unsigned exponent = 0;
  } fp_acc_;
};

// number is float
// ~~~~~~~~~~~~~~~
template <typename Backend>
void number_matcher<Backend>::number_is_float () {
  if (is_integer_) {
    fp_acc_.whole_part = static_cast<double> (int_acc_);
    is_integer_ = false;
  }
}

// in terminal state
// ~~~~~~~~~~~~~~~~~
template <typename Backend>
bool number_matcher<Backend>::in_terminal_state () const {
  switch (this->get_state ()) {
  case integer_digit_state:
  case frac_state:
  case frac_digit_state:
  case exponent_digit_state:
  case done_state: return true;
  default: return false;
  }
}

// leading minus state
// ~~~~~~~~~~~~~~~~~~~
template <typename Backend>
bool number_matcher<Backend>::do_leading_minus_state (parser<Backend> &parser,
                                                      char32_t c) {
  bool match = true;
  if (c == '-') {
    this->set_state (integer_initial_digit_state);
    is_neg_ = true;
  } else if (c == '+') {
    assert (parser.extension_enabled (extensions::leading_plus));
    this->set_state (integer_initial_digit_state);
  } else if (c >= '0' && c <= '9') {
    this->set_state (integer_initial_digit_state);
    match = do_integer_initial_digit_state (parser, c);
  } else {
    unreachable ();
    // minus MUST be followed by the 'int' production.
    this->set_error (parser, error::number_out_of_range);
  }
  return match;
}

// frac state
// ~~~~~~~~~~
template <typename Backend>
bool number_matcher<Backend>::do_frac_state (parser<Backend> &parser,
                                             char32_t const c) {
  bool match = true;
  if (c == '.') {
    this->set_state (frac_initial_digit_state);
  } else if (c == 'e' || c == 'E') {
    this->set_state (exponent_sign_state);
  } else if (c >= '0' && c <= '9') {
    // digits are definitely not part of the next token so we can issue an error
    // right here.
    this->set_error (parser, error::number_out_of_range);
  } else {
    // the 'frac' production is optional.
    match = false;
    this->complete (parser);
  }
  return match;
}

// frac digit
// ~~~~~~~~~~
template <typename Backend>
bool number_matcher<Backend>::do_frac_digit_state (parser<Backend> &parser,
                                                   char32_t const c) {
  assert (this->get_state () == frac_initial_digit_state ||
          this->get_state () == frac_digit_state);

  bool match = true;
  if (c == 'e' || c == 'E') {
    this->number_is_float ();
    if (this->get_state () == frac_initial_digit_state) {
      this->set_error (parser, error::unrecognized_token);
    } else {
      this->set_state (exponent_sign_state);
    }
  } else if (c >= '0' && c <= '9') {
    this->number_is_float ();
    fp_acc_.frac_part = fp_acc_.frac_part * 10 + (c - '0');
    fp_acc_.frac_scale *= 10;

    this->set_state (frac_digit_state);
  } else {
    if (this->get_state () == frac_initial_digit_state) {
      this->set_error (parser, error::unrecognized_token);
    } else {
      match = false;
      this->complete (parser);
    }
  }
  return match;
}

// exponent sign state
// ~~~~~~~~~~~~~~~~~~~
template <typename Backend>
bool number_matcher<Backend>::do_exponent_sign_state (parser<Backend> &parser,
                                                      char32_t c) {
  bool match = true;
  this->number_is_float ();
  this->set_state (exponent_initial_digit_state);
  switch (c) {
  case '+': fp_acc_.exp_is_negative = false; break;
  case '-': fp_acc_.exp_is_negative = true; break;
  default: match = this->do_exponent_digit_state (parser, c); break;
  }
  return match;
}

// complete
// ~~~~~~~~
template <typename Backend>
void number_matcher<Backend>::complete (parser<Backend> &parser) {
  this->set_state (done_state);
  this->make_result (parser);
}

// exponent digit
// ~~~~~~~~~~~~~~
template <typename Backend>
bool number_matcher<Backend>::do_exponent_digit_state (parser<Backend> &parser,
                                                       char32_t const c) {
  assert (this->get_state () == exponent_digit_state ||
          this->get_state () == exponent_initial_digit_state);
  assert (!is_integer_);

  bool match = true;
  if (c >= '0' && c <= '9') {
    fp_acc_.exponent = fp_acc_.exponent * 10U + static_cast<unsigned> (c - '0');
    this->set_state (exponent_digit_state);
  } else {
    if (this->get_state () == exponent_initial_digit_state) {
      this->set_error (parser, error::unrecognized_token);
    } else {
      match = false;
      this->complete (parser);
    }
  }
  return match;
}

// do integer initial digit state
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
template <typename Backend>
bool number_matcher<Backend>::do_integer_initial_digit_state (
    parser<Backend> &parser, char32_t const c) {
  assert (this->get_state () == integer_initial_digit_state);
  assert (is_integer_);
  if (c == '0') {
    this->set_state (frac_state);
  } else if (c >= '1' && c <= '9') {
    assert (int_acc_ == 0);
    int_acc_ = static_cast<unsigned> (c - '0');
    this->set_state (integer_digit_state);
  } else {
    this->set_error (parser, error::unrecognized_token);
  }
  return true;
}

// do integer digit state
// ~~~~~~~~~~~~~~~~~~~~~~
template <typename Backend>
bool number_matcher<Backend>::do_integer_digit_state (parser<Backend> &parser,
                                                      char32_t const c) {
  assert (this->get_state () == integer_digit_state);
  assert (is_integer_);

  bool match = true;
  if (c == '.') {
    this->set_state (frac_initial_digit_state);
    number_is_float ();
  } else if (c == 'e' || c == 'E') {
    this->set_state (exponent_sign_state);
    number_is_float ();
  } else if (c >= '0' && c <= '9') {
    std::uint64_t const new_acc =
        int_acc_ * 10U + static_cast<unsigned> (c - '0');
    if (new_acc < int_acc_) {  // Did this overflow?
      this->set_error (parser, error::number_out_of_range);
    } else {
      int_acc_ = new_acc;
    }
  } else {
    match = false;
    this->complete (parser);
  }
  return match;
}

// consume
// ~~~~~~~
template <typename Backend>
std::pair<typename matcher<Backend>::pointer, bool>
number_matcher<Backend>::consume (parser<Backend> &parser,
                                  std::optional<char32_t> ch) {
  bool match = true;
  if (ch) {
    auto const c = *ch;
    switch (this->get_state ()) {
    case leading_minus_state:
      match = this->do_leading_minus_state (parser, c);
      break;
    case integer_initial_digit_state:
      match = this->do_integer_initial_digit_state (parser, c);
      break;
    case integer_digit_state:
      match = this->do_integer_digit_state (parser, c);
      break;
    case frac_state: match = this->do_frac_state (parser, c); break;
    case frac_initial_digit_state:
    case frac_digit_state: match = this->do_frac_digit_state (parser, c); break;
    case exponent_sign_state:
      match = this->do_exponent_sign_state (parser, c);
      break;
    case exponent_initial_digit_state:
    case exponent_digit_state:
      match = this->do_exponent_digit_state (parser, c);
      break;
    case done_state:
    default: unreachable (); break;
    }
  } else {
    assert (!parser.has_error ());
    if (!this->in_terminal_state ()) {
      this->set_error (parser, error::expected_digits);
    }
    this->complete (parser);
  }
  return {matcher<Backend>::null_pointer (), match};
}

// make result
// ~~~~~~~~~~~
template <typename Backend>
void number_matcher<Backend>::make_result (parser<Backend> &parser) {
  if (parser.has_error ()) {
    return;
  }
  assert (this->in_terminal_state ());

  if (is_integer_) {
    constexpr auto min = std::numeric_limits<std::int64_t>::min ();
    constexpr auto umin = static_cast<std::uint64_t> (min);

    if (is_neg_) {
      if (int_acc_ > umin) {
        this->set_error (parser, error::number_out_of_range);
        return;
      }

      this->set_error (
          parser,
          parser.backend ().int64_value (
              (int_acc_ == umin) ? min
                                 : -static_cast<std::int64_t> (int_acc_)));
      return;
    }
    this->set_error (parser, parser.backend ().uint64_value (int_acc_));
    return;
  }

  auto xf = (fp_acc_.whole_part + fp_acc_.frac_part / fp_acc_.frac_scale);
  auto exp = std::pow (10, fp_acc_.exponent);
  if (std::isinf (exp)) {
    this->set_error (parser, error::number_out_of_range);
    return;
  }
  if (fp_acc_.exp_is_negative) {
    exp = 1.0 / exp;
  }

  xf *= exp;
  if (is_neg_) {
    xf = -xf;
  }

  if (std::isinf (xf) || std::isnan (xf)) {
    this->set_error (parser, error::number_out_of_range);
    return;
  }
  this->set_error (parser, parser.backend ().double_value (xf));
}

//*     _       _            *
//*  __| |_ _ _(_)_ _  __ _  *
//* (_-<  _| '_| | ' \/ _` | *
//* /__/\__|_| |_|_||_\__, | *
//*                   |___/  *
//-MARK:string
template <typename Backend>
class string_matcher final : public matcher<Backend> {
public:
  explicit string_matcher (u8string *const str, bool object_key,
                           char32_t enclosing_char) noexcept
      : matcher<Backend> (start_state),
        is_object_key_{object_key},
        enclosing_char_{enclosing_char},
        str_{str} {
    assert (str != nullptr);
    str->clear ();
  }
  string_matcher (string_matcher const &) = delete;
  string_matcher (string_matcher &&) noexcept = default;

  ~string_matcher () noexcept override = default;

  string_matcher &operator= (string_matcher const &) = delete;
  string_matcher &operator= (string_matcher &&) noexcept = default;

  std::pair<typename matcher<Backend>::pointer, bool> consume (
      parser<Backend> &parser, std::optional<char32_t> ch) override;

private:
  using matcher<Backend>::null_pointer;

  enum state {
    done_state = matcher<Backend>::done,
    start_state,
    normal_char_state,
    escape_state,
    hex1_state,
    hex2_state,
    hex3_state,
    hex4_state,
  };

#if 0
  class appender {
  public:
    constexpr explicit appender (u8string *const result) noexcept
        : result_{result} {
      assert (result != nullptr);
    }
    bool append32 (char32_t code_point);
    bool append16 (char16_t cu);
    u8string *result () { return result_; }
    bool has_high_surrogate () const noexcept { return high_surrogate_ != 0; }

  private:
    u8string *const result_;
    t16_32
  };
#endif

  std::variant<state, std::error_code> consume_normal (parser<Backend> &p,
                                                       bool is_object_key,
                                                       char32_t enclosing_char,
                                                       char32_t code_point);

  /// Process a single "normal" (i.e. not part of an escape or hex sequence)
  /// character. This function wraps consume_normal(). That function does the
  /// real work but this wrapper performs any necessary mutations of the state
  /// machine.
  ///
  /// \param p  The parent parser instance.
  /// \param code_point  The Unicode character being processed.
  void normal (parser<Backend> &p, char32_t code_point);

  /// Adds a single hexadecimal character to \p value.
  ///
  /// \param c  The Unicode code point of the character to be added.
  /// \param value  The initial value.
  /// \returns  If \p c represents a valid hexadecimal character (0..9,A-F,a-f)
  ///   returns \p value * 16 plus the decoded value of \p c. Returns an
  ///   optional with no value if \p c is not a valid hexadecimal character.
  static std::optional<unsigned> hex_value (char32_t c, unsigned value);

  std::variant<error, std::tuple<unsigned, enum state>> consume_hex (
      unsigned hex, enum state state, char32_t code_point);
  void hex (parser<Backend> &p, char32_t code_point);

  std::variant<state, error> consume_escape_state (char32_t code_point);
  void escape (parser<Backend> &p, char32_t code_point);

  static constexpr bool is_hex_state (enum state const state) noexcept {
    return state == hex1_state || state == hex2_state || state == hex3_state ||
           state == hex4_state;
  }

  bool is_object_key_;
  char32_t enclosing_char_;
  u8string *const str_;  // output
  unsigned hex_ = 0U;
  icubaby::t32_8 utf_32_to_8_;
  icubaby::t16_8 utf_16_to_8_;
};

// consume normal
// ~~~~~~~~~~~~~~
template <typename Backend>
auto string_matcher<Backend>::consume_normal (parser<Backend> &p,
                                              bool is_object_key,
                                              char32_t enclosing_char,
                                              char32_t code_point)
    -> std::variant<state, std::error_code> {
  if (code_point == enclosing_char) {
    if (utf_16_to_8_.partial ()) {
      utf_16_to_8_.end_cp (std::back_inserter (*str_));
      assert (!utf_16_to_8_.well_formed ());
      return error::bad_unicode_code_point;
    }
    // Consume the closing quote character.
    auto &n = p.backend ();
    u8string_view const &result = *str_;
    if (std::error_code const error =
            is_object_key ? n.key (result) : n.string_value (result)) {
      return error;
    }
    return done_state;
  }
  if (code_point == char_set::reverse_solidus) {
    return escape_state;
  }
  if (code_point <= 0x1F) {
    // Control characters U+0000 through U+001F MUST be escaped.
    return error::bad_unicode_code_point;
  }

  // Remember this character.
  auto it = utf_32_to_8_ (code_point, std::back_inserter (*str_));
  utf_32_to_8_.end_cp (it);
  if (!utf_32_to_8_.well_formed ()) {
    return error::bad_unicode_code_point;
  }
  return normal_char_state;
}

// normal
// ~~~~~~
template <typename Backend>
void string_matcher<Backend>::normal (parser<Backend> &p, char32_t code_point) {
  std::visit (
      [this, &p] (auto &&arg) {
        using T = std::decay_t<decltype (arg)>;
        if constexpr (std::is_same_v<T, std::error_code>) {
          this->set_error (p, arg);
        } else if constexpr (std::is_same_v<T, state>) {
          this->set_state (arg);
        } else {
          static_assert (always_false<T>, "non-exhaustive visitor");
        }
      },
      this->consume_normal (p, is_object_key_, enclosing_char_, code_point));
}

// hex value [static]
// ~~~~~~~~~
template <typename Backend>
std::optional<unsigned> string_matcher<Backend>::hex_value (
    char32_t const c, unsigned const value) {
  auto digit = 0U;
  if (c >= '0' && c <= '9') {
    digit = static_cast<unsigned> (c) - '0';
  } else if (c >= 'a' && c <= 'f') {
    digit = static_cast<unsigned> (c) - 'a' + 10U;
  } else if (c >= 'A' && c <= 'F') {
    digit = static_cast<unsigned> (c) - 'A' + 10U;
  } else {
    return {std::nullopt};
  }
  return {16U * value + digit};
}

// consume hex
// ~~~~~~~~~~~
template <typename Backend>
auto string_matcher<Backend>::consume_hex (unsigned const hex,
                                           enum state const state,
                                           char32_t const code_point)
    -> std::variant<error, std::tuple<unsigned, enum state>> {

  assert (is_hex_state (state));
  auto const opt_value = hex_value (code_point, hex);
  if (!opt_value) {
    return error::invalid_hex_char;
  }
  switch (state) {
  case hex1_state:
  case hex2_state:
  case hex3_state:
    assert (is_hex_state (static_cast<enum state> (state + 1)));
    return std::make_tuple (*opt_value, static_cast<enum state> (state + 1));

  case hex4_state:
    // We're done with the hex characters and are switching back to the
    // 'normal' state. The means that we can add the accumulated code-point.
    utf_16_to_8_ (static_cast<char16_t> (*opt_value), std::back_inserter (*str_));
    if (!utf_16_to_8_.well_formed ()) {
      return error::bad_unicode_code_point;
    }
    return std::make_tuple (0U, normal_char_state);

  case done_state:
  case start_state:
  case normal_char_state:
  case escape_state: break;
  }

  unreachable ();
  return error::invalid_hex_char;
}

template <typename Backend>
void string_matcher<Backend>::hex (parser<Backend> &p, char32_t code_point) {
  std::visit (
      [this, &p] (auto &&arg) {
        using T = std::decay_t<decltype (arg)>;
        if constexpr (std::is_same_v<T, error>) {
          this->set_error (p, arg);
        } else if constexpr (std::is_same_v<T, std::tuple<unsigned, state>>) {
          hex_ = std::get<unsigned> (arg);
          this->set_state (std::get<state> (arg));
        } else {
          static_assert (always_false<T>, "non-exhaustive visitor");
        }
      },
      this->consume_hex (hex_, static_cast<state> (this->get_state ()),
                         code_point));
}

// consume escape state
// ~~~~~~~~~~~~~~~~~~~~
template <typename Backend>
auto string_matcher<Backend>::consume_escape_state (char32_t code_point)
    -> std::variant<state, error> {
  state next_state = normal_char_state;
  switch (code_point) {
  case char_set::quotation_mark:
  case char_set::solidus:
  case char_set::reverse_solidus:
    // code points are appended as-is.
    break;
  case char_set::latin_small_letter_b: code_point = char_set::backspace; break;
  case char_set::latin_small_letter_f: code_point = char_set::form_feed; break;
  case char_set::latin_small_letter_n: code_point = char_set::line_feed; break;
  case char_set::latin_small_letter_r:
    code_point = char_set::carriage_return;
    break;
  case char_set::latin_small_letter_t:
    code_point = char_set::character_tabulation;
    break;
  case char_set::latin_small_letter_u: return {hex1_state};
  default: return {error::invalid_escape_char};
  }
  assert (next_state == normal_char_state);
  utf_32_to_8_ (code_point, std::back_inserter (*str_));
  assert (utf_32_to_8_.well_formed ());
  return {next_state};
}

// escape
// ~~~~~~
template <typename Backend>
void string_matcher<Backend>::escape (parser<Backend> &p, char32_t code_point) {
  std::visit (
      [this, &p] (auto &&arg) {
        using T = std::decay_t<decltype (arg)>;
        if constexpr (std::is_same_v<T, error>) {
          this->set_error (p, arg);
        } else if constexpr (std::is_same_v<T, state>) {
          this->set_state (arg);
        } else {
          static_assert (always_false<T>, "non-exhaustive visitor");
        }
      },
      this->consume_escape_state (code_point));
}

// consume
// ~~~~~~~
template <typename Backend>
std::pair<typename matcher<Backend>::pointer, bool>
string_matcher<Backend>::consume (parser<Backend> &parser,
                                  std::optional<char32_t> code_point) {
  if (!code_point) {
    this->set_error (parser, error::expected_close_quote);
    return {null_pointer (), true};
  }

  auto const c = *code_point;
  switch (this->get_state ()) {
  // Matches the opening quote.
  case start_state:
    if (c == enclosing_char_) {
      assert (!utf_16_to_8_.partial ());
      this->set_state (normal_char_state);
    } else {
      this->set_error (parser, error::expected_token);
    }
    break;
  case normal_char_state: this->normal (parser, c); break;
  case escape_state: this->escape (parser, c); break;

  case hex1_state: assert (hex_ == 0U); [[fallthrough]];
  case hex2_state:
  case hex3_state:
  case hex4_state: this->hex (parser, c); break;

  case done_state:
  default: assert (false); break;
  }
  return {null_pointer (), true};
}

//*                          *
//*  __ _ _ _ _ _ __ _ _  _  *
//* / _` | '_| '_/ _` | || | *
//* \__,_|_| |_| \__,_|\_, | *
//*                    |__/  *
//-MARK:array
template <typename Backend>
class array_matcher final : public matcher<Backend> {
public:
  array_matcher () noexcept : matcher<Backend> (start_state) {}
  ~array_matcher () noexcept override = default;

  std::pair<typename matcher<Backend>::pointer, bool> consume (
      parser<Backend> &parser, std::optional<char32_t> ch) override;

private:
  using matcher<Backend>::null_pointer;

  enum state {
    done_state = matcher<Backend>::done,
    start_state,
    first_object_state,
    object_state,
    comma_state,
  };

  void end_array (parser<Backend> &parser);
};

// consume
// ~~~~~~~
template <typename Backend>
std::pair<typename matcher<Backend>::pointer, bool>
array_matcher<Backend>::consume (parser<Backend> &p,
                                 std::optional<char32_t> ch) {
  if (!ch) {
    this->set_error (p, error::expected_array_member);
    return {null_pointer (), true};
  }
  auto const c = *ch;
  switch (this->get_state ()) {
  case start_state:
    assert (c == '[');
    if (this->set_error (p, p.backend ().begin_array ())) {
      break;
    }
    this->set_state (first_object_state);
    // Match this character and consume whitespace before the object (or close
    // bracket).
    return {this->make_whitespace_matcher (p), true};

  case first_object_state:
    if (c == ']') {
      this->end_array (p);
      break;
    }
    [[fallthrough]];
  case object_state:
    this->set_state (comma_state);
    return {this->make_root_matcher (p), false};
    break;
  case comma_state:
    if (isspace (c)) {
      // just consume whitespace before a comma.
      return {this->make_whitespace_matcher (p), false};
    }
    switch (c) {
    case ',':
      this->set_state ((p.extension_enabled (extensions::array_trailing_comma))
                           ? first_object_state
                           : object_state);
      return {this->make_whitespace_matcher (p), true};
    case ']': this->end_array (p); break;
    default: this->set_error (p, error::expected_array_member); break;
    }
    break;
  case done_state:
    assert (false && "consume() should not be called when in the 'done' state");
    break;
  default:
    assert (false && "array_matcher<> has reached an unknown state");
    break;
  }
  return {null_pointer (), true};
}

// end array
// ~~~~~~~~~
template <typename Backend>
void array_matcher<Backend>::end_array (parser<Backend> &parser) {
  this->set_error (parser, parser.backend ().end_array ());
  this->set_state (done_state);
}

//*      _     _        _    *
//*  ___| |__ (_)___ __| |_  *
//* / _ \ '_ \| / -_) _|  _| *
//* \___/_.__// \___\__|\__| *
//*         |__/             *
//-MARK:object
template <typename Backend>
class object_matcher final : public matcher<Backend> {
public:
  object_matcher () noexcept : matcher<Backend> (start_state) {}
  ~object_matcher () noexcept override = default;

  std::pair<typename matcher<Backend>::pointer, bool> consume (
      parser<Backend> &parser, std::optional<char32_t> ch) override;

private:
  using matcher<Backend>::null_pointer;

  enum state {
    done_state = matcher<Backend>::done,
    start_state,
    first_key_state,
    key_state,
    colon_state,
    value_state,
    comma_state,
  };

  void end_object (parser<Backend> &parser);
};

// consume
// ~~~~~~~
template <typename Backend>
std::pair<typename matcher<Backend>::pointer, bool>
object_matcher<Backend>::consume (parser<Backend> &parser,
                                  std::optional<char32_t> ch) {
  if (!ch) {
    this->set_error (parser, error::expected_object_member);
    return {null_pointer (), true};
  }
  auto const c = *ch;
  switch (this->get_state ()) {
  case start_state:
    assert (c == '{');
    this->set_state (first_key_state);
    if (this->set_error (parser, parser.backend ().begin_object ())) {
      break;
    }
    return {this->make_whitespace_matcher (parser), true};
  case first_key_state:
    // We allow either a closing brace (to end the object) or a property name.
    if (c == '}') {
      this->end_object (parser);
      break;
    }
    [[fallthrough]];
  case key_state:
    // Match a property name then expect a colon.
    this->set_state (colon_state);
    return {this->make_root_matcher (parser, true /*object key?*/), false};
  case colon_state:
    if (isspace (c)) {
      // just consume whitespace before the colon.
      return {this->make_whitespace_matcher (parser), false};
    }
    if (c == ':') {
      this->set_state (value_state);
    } else {
      this->set_error (parser, error::expected_colon);
    }
    break;
  case value_state:
    this->set_state (comma_state);
    return {this->make_root_matcher (parser), false};
  case comma_state:
    if (isspace (c)) {
      // just consume whitespace before the comma.
      return {this->make_whitespace_matcher (parser), false};
    }
    if (c == ',') {
      // Strictly conforming JSON requires a property name following a comma but
      // we have an extension to allow an trailing comma which may be followed
      // by the object's closing brace.
      this->set_state (
          (parser.extension_enabled (extensions::object_trailing_comma))
              ? first_key_state
              : key_state);
      // Consume the comma and any whitespace before the close brace or property
      // name.
      return {this->make_whitespace_matcher (parser), true};
    }
    if (c == '}') {
      this->end_object (parser);
    } else {
      this->set_error (parser, error::expected_object_member);
    }
    break;
  case done_state:
  default: assert (false); break;
  }
  // No change of matcher. Consume the input character.
  return {null_pointer (), true};
}

// end object
// ~~~~~~~~~~~
template <typename Backend>
void object_matcher<Backend>::end_object (parser<Backend> &parser) {
  this->set_error (parser, parser.backend ().end_object ());
  this->set_state (done_state);
}

//*             *
//* __ __ _____ *
//* \ V  V (_-< *
//*  \_/\_//__/ *
//*             *
/// This matcher consumes whitespace and updates the row number in response to
/// the various combinations of CR and LF. Supports #, //, and /* style comments
/// as an extension.
//-MARK:whitespace
template <typename Backend>
class whitespace_matcher final : public matcher<Backend> {
public:
  whitespace_matcher () noexcept : matcher<Backend> (body_state) {}
  whitespace_matcher (whitespace_matcher const &) = delete;
  whitespace_matcher (whitespace_matcher &&) noexcept = default;

  ~whitespace_matcher () noexcept override = default;

  whitespace_matcher &operator= (whitespace_matcher const &) = delete;
  whitespace_matcher &operator= (whitespace_matcher &&) noexcept = default;

  std::pair<typename matcher<Backend>::pointer, bool> consume (
      parser<Backend> &parser, std::optional<char32_t> ch) override;

private:
  using matcher<Backend>::null_pointer;

  enum state {
    done_state = matcher<Backend>::done,
    /// Normal whitespace scanning. The "body" is the whitespace being consumed.
    body_state,
    /// Handles the LF part of a Windows-style CR/LF pair.
    crlf_state,
    /// Consumes the contents of a single-line comment.
    single_line_comment_state,
    comment_start_state,
    /// Consumes the contents of a multi-line comment.
    multi_line_comment_body_state,
    /// Entered when checking for the second character of the '*/' pair.
    multi_line_comment_ending_state,
    /// Handles the LF part of a Windows-style CR/LF pair inside a multi-line
    /// comment.
    multi_line_comment_crlf_state,
  };

  std::pair<typename matcher<Backend>::pointer, bool> consume_body (
      parser<Backend> &parser, char32_t c);

  std::pair<typename matcher<Backend>::pointer, bool> consume_comment_start (
      parser<Backend> &parser, char32_t c);

  std::pair<typename matcher<Backend>::pointer, bool> multi_line_comment_body (
      parser<Backend> &parser, char32_t c);

  void cr (parser<Backend> &parser, state next) {
    assert (this->get_state () == multi_line_comment_body_state ||
            this->get_state () == body_state);
    parser.advance_row ();
    this->set_state (next);
  }
  void lf (parser<Backend> &parser) { parser.advance_row (); }

  /// Processes the second character of a Windows-style CR/LF pair. Returns true
  /// if the character shoud be treated as whitespace.
  bool crlf (parser<Backend> &parser, char32_t c) {
    if (c != char_set::line_feed) {
      return false;
    }
    parser.reset_column ();
    return true;
  }
};

// consume
// ~~~~~~~
template <typename Backend>
std::pair<typename matcher<Backend>::pointer, bool>
whitespace_matcher<Backend>::consume (parser<Backend> &parser,
                                      std::optional<char32_t> ch) {
  if (!ch) {
    this->set_state (done_state);
  } else {
    auto const c = *ch;
    switch (this->get_state ()) {
    // Handles the LF part of a Windows-style CR/LF pair.
    case crlf_state:
      this->set_state (body_state);
      if (crlf (parser, c)) {
        break;
      }
      [[fallthrough]];
    case body_state: return this->consume_body (parser, c);
    case comment_start_state: return this->consume_comment_start (parser, c);

    case multi_line_comment_ending_state:
      assert (parser.extension_enabled (extensions::multi_line_comments));
      this->set_state (c == char_set::solidus ? body_state
                                              : multi_line_comment_body_state);
      break;

    case multi_line_comment_crlf_state:
      this->set_state (multi_line_comment_body_state);
      if (crlf (parser, c)) {
        break;
      }
      [[fallthrough]];
    case multi_line_comment_body_state:
      return this->multi_line_comment_body (parser, c);
    case single_line_comment_state:
      assert (parser.extension_enabled (extensions::bash_comments) ||
              parser.extension_enabled (extensions::single_line_comments) ||
              parser.extension_enabled (extensions::multi_line_comments));
      if (c == char_set::carriage_return || c == char_set::line_feed) {
        // This character marks a bash/single-line comment end. Go back to
        // normal whitespace handling. Retry with the same character.
        this->set_state (body_state);
        return {null_pointer (), false};
      }
      // Just consume the character.
      break;

    default: assert (false); break;
    }
  }
  return {null_pointer (), true};
}

// consume body
// ~~~~~~~~~~~~
template <typename Backend>
std::pair<typename matcher<Backend>::pointer, bool>
whitespace_matcher<Backend>::consume_body (parser<Backend> &parser,
                                           char32_t c) {
  auto const stop_retry = [this] () {
    // Stop, pop this matcher, and retry with the same character.
    this->set_state (done_state);
    return std::pair{null_pointer (), false};
  };

  switch (c) {
  case char_set::space: break;  // Just consume.
  case char_set::character_tabulation:
    // TODO: tab expansion.
    break;
  case char_set::carriage_return: this->cr (parser, crlf_state); break;
  case char_set::line_feed: this->lf (parser); break;
  case char_set::number_sign:
    if (!parser.extension_enabled (extensions::bash_comments)) {
      return stop_retry ();
    }
    this->set_state (single_line_comment_state);
    break;
  case char_set::solidus:
    if (!parser.extension_enabled (extensions::single_line_comments) &&
        !parser.extension_enabled (extensions::multi_line_comments)) {
      return stop_retry ();
    }
    this->set_state (comment_start_state);
    break;
  default: return stop_retry ();
  }
  return {null_pointer (), true};  // Consume this character.
}

// consume comment start
// ~~~~~~~~~~~~~~~~~~~~~
/// We've already seen an initial slash ('/') which could mean one of three
/// things:
///   - the start of a single-line // comment
///   - the start of a multi-line /* */ comment
///   - just a random / character.
/// This function handles the character after that initial slash to determine
/// which of the three it is.
template <typename Backend>
std::pair<typename matcher<Backend>::pointer, bool>
whitespace_matcher<Backend>::consume_comment_start (parser<Backend> &parser,
                                                    char32_t c) {
  if (c == char_set::solidus &&
      parser.extension_enabled (extensions::single_line_comments)) {
    this->set_state (single_line_comment_state);
  } else if (c == char_set::asterisk &&
             parser.extension_enabled (extensions::multi_line_comments)) {
    this->set_state (multi_line_comment_body_state);
  } else {
    this->set_error (parser, error::expected_token);
  }
  return {null_pointer (), true};  // Consume this character.
}

// multi line comment body
// ~~~~~~~~~~~~~~~~~~~~~~~
/// Similar to consume_body() except that the commented characters are consumed
/// as well as whitespace. We're looking to see a star ('*') character which may
/// indicate the end of the multi-line comment.
template <typename Backend>
std::pair<typename matcher<Backend>::pointer, bool>
whitespace_matcher<Backend>::multi_line_comment_body (parser<Backend> &parser,
                                                      char32_t c) {
  assert (parser.extension_enabled (extensions::multi_line_comments));
  assert (this->get_state () == multi_line_comment_body_state);
  switch (c) {
  case char_set::asterisk:
    // This could be a standalone star character or be followed by a slash
    // to end the multi-line comment.
    this->set_state (multi_line_comment_ending_state);
    break;
  case char_set::carriage_return:
    this->cr (parser, multi_line_comment_crlf_state);
    break;
  case char_set::line_feed: this->lf (parser); break;
  case char_set::character_tabulation: break;  // TODO: tab expansion.
  default: break;             // Just consume.
  }
  return {null_pointer (), true};  // Consume this character.
}

//*           __  *
//*  ___ ___ / _| *
//* / -_) _ \  _| *
//* \___\___/_|   *
//*               *
//-MARK:eof
template <typename Backend>
class eof_matcher final : public matcher<Backend> {
public:
  eof_matcher () noexcept : matcher<Backend> (start_state) {}
  ~eof_matcher () noexcept override = default;

  std::pair<typename matcher<Backend>::pointer, bool> consume (
      parser<Backend> &parser, std::optional<char32_t> ch) override;

private:
  enum state {
    done_state = matcher<Backend>::done,
    start_state,
  };
};

// consume
// ~~~~~~~
template <typename Backend>
std::pair<typename matcher<Backend>::pointer, bool>
eof_matcher<Backend>::consume (parser<Backend> &parser,
                               std::optional<char32_t> const ch) {
  if (ch) {
    this->set_error (parser, error::unexpected_extra_input);
  } else {
    this->set_state (done_state);
  }
  return {matcher<Backend>::null_pointer (), true};
}

//*               _                _      _             *
//*  _ _ ___  ___| |_   _ __  __ _| |_ __| |_  ___ _ _  *
//* | '_/ _ \/ _ \  _| | '  \/ _` |  _/ _| ' \/ -_) '_| *
//* |_| \___/\___/\__| |_|_|_\__,_|\__\__|_||_\___|_|   *
//*                                                     *
//-MARK:root
template <typename Backend>
class root_matcher final : public matcher<Backend> {
public:
  explicit constexpr root_matcher (bool const is_object_key = false) noexcept
      : matcher<Backend> (start_state), object_key_{is_object_key} {}
  ~root_matcher () noexcept override = default;

  std::pair<typename matcher<Backend>::pointer, bool> consume (
      parser<Backend> &parser, std::optional<char32_t> ch) override;

private:
  using matcher<Backend>::null_pointer;

  enum state {
    done_state = matcher<Backend>::done,
    start_state,
    new_token_state,
  };
  bool const object_key_;
};

// consume
// ~~~~~~~
template <typename Backend>
std::pair<typename matcher<Backend>::pointer, bool>
root_matcher<Backend>::consume (parser<Backend> &parser,
                                std::optional<char32_t> ch) {
  if (!ch) {
    this->set_error (parser, error::expected_token);
    return {null_pointer (), true};
  }

  using pointer = typename matcher<Backend>::pointer;
  using deleter = typename pointer::deleter_type;
  switch (this->get_state ()) {
  case start_state:
    this->set_state (new_token_state);
    return {this->make_whitespace_matcher (parser), false};

  case new_token_state: {
    if (object_key_ && *ch != '"' && *ch != '\'') {
      this->set_error (parser, error::expected_string);
      // Don't return here in order to allow the switch default to produce a
      // different error code for a bad token.
    }
    this->set_state (done_state);
    switch (*ch) {
    case '+':
      if (!parser.extension_enabled (extensions::leading_plus)) {
        this->set_error (parser, error::expected_token);
        return {null_pointer (), true};
      }
      [[fallthrough]];

    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      return {this->template make_terminal_matcher<number_matcher<Backend>> (
                  parser),
              false};
    case '\'':
      if (!parser.extension_enabled (extensions::single_quote_string)) {
        this->set_error (parser, error::expected_token);
        return {null_pointer (), true};
      }
      [[fallthrough]];
    case '"':
      return {this->template make_terminal_matcher<string_matcher<Backend>> (
                  parser, &parser.string_, object_key_, *ch),
              false};
    case 't':
      return {
          this->template make_terminal_matcher<true_token_matcher<Backend>> (
              parser),
          false};
    case 'f':
      return {
          this->template make_terminal_matcher<false_token_matcher<Backend>> (
              parser),
          false};
    case 'n':
      return {
          this->template make_terminal_matcher<null_token_matcher<Backend>> (
              parser),
          false};
    case '[':
      return {pointer{new array_matcher<Backend> (),
                      deleter{deleter::mode::do_delete}},
              false};
    case '{':
      return {pointer{new object_matcher<Backend> (),
                      deleter{deleter::mode::do_delete}},
              false};
    default:
      this->set_error (parser, error::expected_token);
      return {null_pointer (), true};
    }
  } break;
  default: unreachable (); break;
  }
  assert (false);  // unreachable.
  return {null_pointer (), true};
}

//*     _           _     _                 _                          *
//*  __(_)_ _  __ _| |___| |_ ___ _ _    __| |_ ___ _ _ __ _ __ _ ___  *
//* (_-< | ' \/ _` | / -_)  _/ _ \ ' \  (_-<  _/ _ \ '_/ _` / _` / -_) *
//* /__/_|_||_\__, |_\___|\__\___/_||_| /__/\__\___/_| \__,_\__, \___| *
//*           |___/                                         |___/      *
//-MARK:singleton storage
template <typename Backend>
struct singleton_storage {
  template <typename T>
  struct storage {
    using type = typename std::aligned_storage_t<sizeof (T), alignof (T)>;
  };

  typename storage<eof_matcher<Backend>>::type eof;
  typename storage<whitespace_matcher<Backend>>::type trailing_ws;
  typename storage<root_matcher<Backend>>::type root;

  std::variant<details::number_matcher<Backend>,
               details::string_matcher<Backend>,
               details::true_token_matcher<Backend>,
               details::false_token_matcher<Backend>,
               details::null_token_matcher<Backend>,
               details::whitespace_matcher<Backend>>
      terminals_;
};

}  // namespace peejay

// (ctor)
// ~~~~~~
template <typename Backend>
PEEJAY_CXX20REQUIRES (backend<Backend>)
template <typename OtherBackend>
PEEJAY_CXX20REQUIRES (backend<OtherBackend>)
parser<Backend>::parser (OtherBackend &&backend, extensions const extensions)
    : extensions_{extensions}, backend_{std::forward<OtherBackend> (backend)} {
  using mpointer = typename matcher::pointer;
  using deleter = typename mpointer::deleter_type;
  // The EOF matcher is placed at the bottom of the stack to ensure that the
  // input JSON ends after a single top-level object.
  stack_.push (mpointer (new (&singletons_.eof) details::eof_matcher<Backend>{},
                         deleter{deleter::mode::do_dtor}));
  // We permit whitespace after the top-level object.
  stack_.push (mpointer (new (&singletons_.trailing_ws)
                             details::whitespace_matcher<Backend>{},
                         deleter{deleter::mode::do_dtor}));
  stack_.push (this->make_root_matcher ());
}

// make root matcher
// ~~~~~~~~~~~~~~~~~
template <typename Backend>
PEEJAY_CXX20REQUIRES (backend<Backend>)
auto parser<Backend>::make_root_matcher (bool object_key) -> pointer {
  using root_matcher = details::root_matcher<Backend>;
  using deleter = typename pointer::deleter_type;
  return pointer (new (&singletons_.root) root_matcher (object_key),
                  deleter{deleter::mode::do_dtor});
}

// make whitespace matcher
// ~~~~~~~~~~~~~~~~~~~~~~~
template <typename Backend>
PEEJAY_CXX20REQUIRES (backend<Backend>)
auto parser<Backend>::make_whitespace_matcher () -> pointer {
  using whitespace_matcher = details::whitespace_matcher<Backend>;
  return this->make_terminal_matcher<whitespace_matcher> ();
}

// input
// ~~~~~
template <typename Backend>
PEEJAY_CXX20REQUIRES (backend<Backend>)
template <typename InputIterator>
PEEJAY_CXX20REQUIRES (
    (std::input_iterator<InputIterator> &&
     std::is_same_v<
         std::decay_t<typename std::iterator_traits<InputIterator>::value_type>,
         char8_t>))
auto parser<Backend>::input (InputIterator first, InputIterator last)
    -> parser & {
  if (error_) {
    return *this;
  }

  auto code_point = char32_t{0};
  while (first != last && !error_) {
    auto *const it = utf_ (*first, &code_point);
    assert (it == &code_point || it == &code_point + 1);
    ++first;
    if (it != &code_point) {
      this->consume_code_point (code_point);
      if (!error_) {
        this->advance_column ();
      }
    }
  }
  return *this;
}

// consume code point
// ~~~~~~~~~~~~~~~~~~
template <typename Backend>
PEEJAY_CXX20REQUIRES (backend<Backend>)
void parser<Backend>::consume_code_point (char32_t code_point) {
  bool retry = false;
  do {
    assert (!stack_.empty ());
    auto &handler = stack_.top ();
    auto res = handler->consume (*this, code_point);
    if (error_) {
      return;
    }
    if (handler->is_done ()) {
      stack_.pop ();  // release the topmost matcher object.
      matcher_pos_ = pos_;
    }

    if (res.first != nullptr) {
      if (stack_.size () > max_stack_depth_) {
        // We've already hit the maximum allowed parse stack depth. Reject this
        // new matcher.
        assert (!error_);
        error_ = error::nesting_too_deep;
        return;
      }

      stack_.push (std::move (res.first));
      matcher_pos_ = pos_;
    }
    retry = !res.second;
  } while (retry);
}

// eof
// ~~~
template <typename Backend>
PEEJAY_CXX20REQUIRES (backend<Backend>)
decltype (auto) parser<Backend>::eof () {
  while (!stack_.empty () && !has_error ()) {
    auto &handler = stack_.top ();
    auto res = handler->consume (*this, std::optional<char>{std::nullopt});
    assert (handler->is_done ());
    assert (res.second);
    stack_.pop ();  // release the topmost matcher object.
  }
  return this->backend ().result ();
}

}  // namespace peejay

#endif  // PEEJAY_JSON_HPP
