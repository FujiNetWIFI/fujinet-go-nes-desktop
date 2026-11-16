/*
 * Condition text helpers, with no engine behind them: the words a
 * breakpoint condition adds to MesenCE's grammar, and a syntax check Mesen's
 * evaluator does not make (it forgives "value ==" and "(a").
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NES_EXPR_H
#define NES_EXPR_H

#include <string>

// Replace the whole word `word` (lower case; matched in any case) in `text`
// by `with`, outside of longer names. *found says whether it occurred.
std::string replace_word(const std::string& text, const char* word, const std::string& with, bool* found);

// The text MesenCE gets for a breakpoint condition in an address space
// (nesdebug_space): `old` becomes [address], the byte at the access's
// address, which is still the old one when the breakpoint is checked (the
// store comes after). Not in a PPU space, where [..] reads the CPU bus: false
// with the reason in err.
bool translate_condition(int space, const std::string& cond, std::string& out, std::string& err);

// Well-formedness: balanced (), [], {}; no operator without its operands;
// no trailing operator. True for the empty string. On false, why says what.
bool syntax_ok(const std::string& text, std::string& why);

#endif
