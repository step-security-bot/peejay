#!/usr/bin/python3
# ===- cprun/parse.py -----------------------------------------------------===//
# *                             *
# *  _ __   __ _ _ __ ___  ___  *
# * | '_ \ / _` | '__/ __|/ _ \ *
# * | |_) | (_| | |  \__ \  __/ *
# * | .__/ \__,_|_|  |___/\___| *
# * |_|                         *
# ===----------------------------------------------------------------------===//
#
#  Distributed under the Apache License v2.0.
#  See https://github.com/paulhuggett/peejay/blob/main/LICENSE.TXT
#  for license information.
#  SPDX-License-Identifier: Apache-2.0
#
# ===----------------------------------------------------------------------===//
"""
This module is used to generate the table of Unicode code point categories
that are used to parse tokens in Peejay.
"""

from collections.abc import MutableSequence
from enum import Enum
from typing import Annotated, Generator, Optional, Sequence
import argparse
import pathlib

from unicode_data import CodePoint, CodePointValueDict, DbDict, GeneralCategory, MAX_CODE_POINT, read_unicode_data

CodePointBitsType = Annotated[
    int, 'The number of bits used to represent a code point']
CODE_POINT_BITS: CodePointBitsType = 21

RunLengthBitsType = Annotated[
    int, 'The number of bits used to represent a run length']
RUN_LENGTH_BITS: RunLengthBitsType = 9
MAX_RUN_LENGTH = pow(2, RUN_LENGTH_BITS) - 1

RuleBitsType = Annotated[int, 'The number of bits used to represent a rule']
RULE_BITS: RuleBitsType = 2
MAX_RULE = pow(2, RULE_BITS) - 1


class GrammarRule(Enum):
    whitespace = 0b00
    identifier_start = 0b01
    identifier_part = 0b11


CATEGORY_TO_GRAMMAR_RULE: dict[GeneralCategory, GrammarRule] = {
    GeneralCategory.Spacing_Mark: GrammarRule.identifier_part,
    GeneralCategory.Connector_Punctuation: GrammarRule.identifier_part,
    GeneralCategory.Decimal_Number: GrammarRule.identifier_part,
    GeneralCategory.Letter_Number: GrammarRule.identifier_start,
    GeneralCategory.Lowercase_Letter: GrammarRule.identifier_start,
    GeneralCategory.Modifier_Letter: GrammarRule.identifier_start,
    GeneralCategory.Nonspacing_Mark: GrammarRule.identifier_part,
    GeneralCategory.Space_Separator: GrammarRule.whitespace,
    GeneralCategory.Other_Letter: GrammarRule.identifier_start,
    GeneralCategory.Titlecase_Letter: GrammarRule.identifier_start,
    GeneralCategory.Uppercase_Letter: GrammarRule.identifier_start,
}


class OutputRow:
    """An individual output row representing a run of unicode code points
    which all belong to the same rule."""

    def __init__(self, code_point: CodePoint, length: int,
                 category: GrammarRule):
        assert code_point < pow(2, CODE_POINT_BITS)
        self.__code_point: CodePoint = code_point
        assert length < pow(2, RUN_LENGTH_BITS)
        self.__length: int = length
        assert category.value < pow(2, RULE_BITS)
        self.__category: GrammarRule = category

    def as_str(self, db: DbDict) -> str:
        return '{{ 0x{0:04x}, {1}, {2} }}, // {3} ({4})'\
            .format(self.__code_point, self.__length, self.__category.value,
                    db[self.__code_point]['Name'], self.__category.name)


class CRAState:
    """A class used to model the state of code_run_array() as it processes
    each unicode code point in sequence."""

    def __init__(self) -> None:
        self.__first: CodePoint = CodePoint(0)
        self.__length: int = 0
        self.__max_length: int = 0
        self.__cat: Optional[GrammarRule] = None

    def new_run(self, code_point: CodePoint,
                mc: Optional[GrammarRule]) -> None:
        """Start a new potential run of contiguous code points.

        :param code_point: The code point at which the run may start.
        :param mc: The rule associated with this code point or None.
        :return: None
        """

        self.__first = code_point
        self.__cat = mc
        self.__length = int(mc is not None)

    def extend_run(self, mc: Optional[GrammarRule]) -> bool:
        """Adds a code point to the current run or indicates that a new run
        should be started.

        :param mc:
        :return: True if the current run was extended or False if a new run
                 should be started.
        """

        if self.__cat is not None and mc == self.__cat:
            if self.__length > MAX_RUN_LENGTH:
                return False
            self.__length += 1
            self.__max_length = max(self.__max_length, self.__length)
            return True
        return False

    def end_run(
            self,
            result: MutableSequence[OutputRow]) -> MutableSequence[OutputRow]:
        """Call at the end of a contiguous run of entries. If there are new
        values, a new Entry will be appended to the 'result' sequence.

        :param result: The sequence to which a new entry may be appended.
        :return: The value 'result'.
        """

        if self.__length > 0 and self.__cat is not None:
            assert self.__first <= MAX_CODE_POINT
            assert self.__length <= MAX_RUN_LENGTH
            assert self.__cat.value <= MAX_RULE
            result.append(OutputRow(self.__first, self.__length, self.__cat))
        return result

    def max_run(self) -> int:
        """The length of the longest contiguous run that has been encountered."""
        return self.__max_length


def all_code_points() -> Generator[CodePoint, None, None]:
    """A generator which yields all of the valid Unicode code points in
    sequence."""

    code_point = CodePoint(0)
    while code_point <= MAX_CODE_POINT:
        yield code_point
        code_point = CodePoint(code_point + 1)


def code_run_array(db: DbDict) -> list[OutputRow]:
    """Produces an array of code runs from the Unicode database dictionary.

    :param db: The Unicode database dictionary.
    :return: An array of code point runs.
    """

    state = CRAState()
    code_runs: list[OutputRow] = list()
    for code_point in all_code_points():
        mc: Optional[GrammarRule] = None
        v: Optional[CodePointValueDict] = db.get(code_point)
        if v is not None:
            mc = CATEGORY_TO_GRAMMAR_RULE.get(v['General_Category'])
            if state.extend_run(mc):
                continue
        state.end_run(code_runs)
        state.new_run(code_point, mc)
    state.end_run(code_runs)
    return code_runs


def patch_special_code_points(db: DbDict) -> DbDict:
    """The ECMAScript grammar rules assigns meaning to some individual Unicode
    code points as well as to entire categories of code point. This function
    ensures that the individual code points belong to categories that will in
    turn be mapped to the correct grammar_rule value.

    :param db: The Unicode database dictionary.
    :return: The Unicode database dictionary.
    """

    # Check that the GeneralCategory enumerations will be eventually mapped to
    # the grammar_rule value we expect.
    assert CATEGORY_TO_GRAMMAR_RULE[
        GeneralCategory.Space_Separator] == GrammarRule.whitespace
    assert CATEGORY_TO_GRAMMAR_RULE[
        GeneralCategory.Other_Letter] == GrammarRule.identifier_start
    assert CATEGORY_TO_GRAMMAR_RULE[
        GeneralCategory.Spacing_Mark] == GrammarRule.identifier_part
    special_code_points = {
        CodePoint(0x0009): GeneralCategory.Space_Separator,
        CodePoint(0x000A): GeneralCategory.Space_Separator,
        CodePoint(0x000B): GeneralCategory.Space_Separator,
        CodePoint(0x000C): GeneralCategory.Space_Separator,
        CodePoint(0x000D): GeneralCategory.Space_Separator,
        CodePoint(0x0020): GeneralCategory.Space_Separator,
        CodePoint(0x0024): GeneralCategory.Other_Letter,
        CodePoint(0x005F): GeneralCategory.Other_Letter,
        CodePoint(0x00A0): GeneralCategory.Space_Separator,
        CodePoint(0x200C): GeneralCategory.Spacing_Mark,
        CodePoint(0x200D): GeneralCategory.Spacing_Mark,
        CodePoint(0xFEFF): GeneralCategory.Space_Separator
    }
    for cp, cat in special_code_points.items():
        db[cp]['General_Category'] = cat
    return db


def dump_db(db: DbDict) -> None:
    for k, v in db.items():
        print('U+{0:04x} {1}'.format(k, v))


def emit_header(entries: Sequence[OutputRow], include_guard: str) -> None:
    """Emits a C++ header file which declares the array variable along with the
    necessary types.

    :param entries: A sequence of OutputRow instances sorted by code point.
    :param include_guard: The name of the header file include guard to be used.
    :return: None
    """
    print('// This file was auto-generated. DO NOT EDIT!')
    print('#ifndef {0}'.format(include_guard))
    print('#define {0}'.format(include_guard))
    print('''
#include <array>
#include <cstdint>

namespace peejay {

enum class grammar_rule : std::uint8_t {''')
    print(',\n'.join(
        ['  {0} = 0b{1:0>2b}'.format(x.name, x.value) for x in GrammarRule]))
    print('''}};
constexpr auto idmask = 0b01U;
struct cprun {{
  std::uint_least32_t code_point: {0};
  std::uint_least32_t length: {1};
  std::uint_least32_t rule: {2};
}};'''.format(CODE_POINT_BITS, RUN_LENGTH_BITS, RULE_BITS))
    assert CODE_POINT_BITS + RUN_LENGTH_BITS + RULE_BITS <= 32

    print('inline std::array<cprun, {0}> const code_point_runs = {{{{'.format(
        len(entries)))
    for x in entries:
        print('  {0}'.format(x.as_str(db)))
    print('}};')
    print('\n} // end namespace peejay')
    print('#endif // {0}'.format(include_guard))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        prog='ProgramName',
        description=
        'Generates C++ source and header files which map Unicode code points to ECMAScript grammar rules'
    )
    parser.set_defaults(emit_header=True)
    parser.add_argument('-u',
                        '--unicode-data',
                        help='the path of the UnicodeData.txt file',
                        default='./UnicodeData.txt',
                        type=pathlib.Path)
    parser.add_argument('--include-guard',
                        help='the name of header file include guard macro',
                        default='CPRUN_HPP')

    group = parser.add_mutually_exclusive_group()
    group.add_argument('-d',
                       '--dump',
                       help='dump the Unicode code point database',
                       action='store_true')

    args = parser.parse_args()
    db = read_unicode_data(args.unicode_data)
    if args.dump:
        dump_db(db)
    else:
        db = patch_special_code_points(db)
        entries = code_run_array(db)
        emit_header(entries, args.include_guard)
