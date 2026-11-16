/*
 * Condition text helpers (core/src/expr.h): no engine, so they run in a test.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cctype>
#include <cstring>
#include <string>

extern "C" {
#include "nesdebug.h"
}

#include "expr.h"

namespace {
bool ident_char(char c)
{
  return isalnum((unsigned char)c) || c == '_';
}

} // namespace

// Replace the whole word `word` (any case) by `with`, outside of other names.
std::string replace_word(const std::string& text, const char* word, const std::string& with, bool* found)
{
  const size_t wlen = strlen(word);
  std::string out;
  for(size_t i = 0; i < text.size();)
  {
    bool hit = i + wlen <= text.size() && (i == 0 || !ident_char(text[i - 1])) &&
               (i + wlen == text.size() || !ident_char(text[i + wlen]));
    if(hit)
      for(size_t k = 0; k < wlen; k++)
        if(tolower((unsigned char)text[i + k]) != word[k]) { hit = false; break; }
    if(hit)
    {
      out += with;
      i += wlen;
      if(found) *found = true;
    }
    else
      out += text[i++];
  }
  return out;
}

// The text Mesen gets: `old` is the byte at the access's address, which is
// still the old one when the breakpoint is checked (the store comes after).
// Not in a PPU space, where [..] reads the CPU bus.
bool translate_condition(int space, const std::string& cond, std::string& out, std::string& err)
{
  bool used = false;
  out = replace_word(cond, "old", "[address]", &used);
  if(used && space != NESDEBUG_SPACE_CPU)
  {
    err = "old is for CPU-space breakpoints; use value and address";
    return false;
  }
  return true;
}

// Mesen's evaluator forgives "value ==" and "(a": a condition must be well
// formed or the breakpoint would silently test garbage.
bool syntax_ok(const std::string& text, std::string& why)
{
  std::string stack;
  bool operand = true;      // the next token must start an operand
  for(size_t i = 0; i < text.size();)
  {
    const char c = text[i];
    if(isspace((unsigned char)c)) { i++; continue; }
    if(isalnum((unsigned char)c) || c == '$' || c == '_' || c == '@' || c == '\'' || c == '.')
    {
      if(!operand) { why = "an operator is missing before '" + std::string(1, c) + "'"; return false; }
      while(i < text.size() && (isalnum((unsigned char)text[i]) || text[i] == '$' || text[i] == '_' ||
                                text[i] == '@' || text[i] == '\'' || text[i] == '.'))
        i++;
      operand = false;
      continue;
    }
    if(c == '(' || c == '[' || c == '{')
    {
      if(!operand) { why = "an operator is missing before '" + std::string(1, c) + "'"; return false; }
      stack += c;
      operand = true;
      i++;
      continue;
    }
    if(c == ')' || c == ']' || c == '}')
    {
      const char open = c == ')' ? '(' : c == ']' ? '[' : '{';
      if(operand) { why = "an operand is missing before '" + std::string(1, c) + "'"; return false; }
      if(stack.empty() || stack.back() != open) { why = "unbalanced '" + std::string(1, c) + "'"; return false; }
      stack.pop_back();
      i++;
      continue;
    }
    // operators
    static const char* const two[] = { "<<", ">>", "<=", ">=", "==", "!=", "&&", "||" };
    size_t len = 1;
    for(const char* t : two)
      if(text.compare(i, 2, t) == 0) len = 2;
    const bool unary = len == 1 && (c == '-' || c == '+' || c == '!' || c == '~');
    const bool binary = len == 2 || strchr("*/%+-<>&|^", c) != nullptr;
    if(!unary && !binary) { why = "unexpected '" + std::string(1, c) + "'"; return false; }
    if(operand)
    {
      if(!unary) { why = "an operand is missing before '" + text.substr(i, len) + "'"; return false; }
    }
    else
    {
      if(!binary) { why = "an operator is missing before '" + std::string(1, c) + "'"; return false; }
      operand = true;
    }
    i += len;
  }
  if(operand && !text.empty()) { why = "the expression ends in an operator"; return false; }
  if(!stack.empty()) { why = "unbalanced '" + std::string(1, stack.back()) + "'"; return false; }
  return true;
}

