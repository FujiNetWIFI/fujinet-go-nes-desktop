/*
 * expr_test -- the condition text helpers (core/src/expr.h), pure: `old` is
 * rewritten to the byte at the access's address (and refused where that is
 * not the CPU bus), the syntax check refuses what MesenCE's evaluator
 * forgives, and a fuzz of garbage never crashes either.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
#include "nesdebug.h"
}

#include "expr.h"

static int failures;
static void check(bool ok, const char* what)
{
  printf("%s: %s\n", ok ? "ok" : "FAIL", what);
  if(!ok) failures++;
}

static bool fine(const char* text)
{
  std::string why;
  return syntax_ok(text, why);
}

static std::string translated(int space, const char* cond, bool* ok = nullptr)
{
  std::string out, err;
  const bool good = translate_condition(space, cond, out, err);
  if(ok) *ok = good;
  return good ? out : err;
}

int main()
{
  // ---- replace_word ----
  {
    bool found = false;
    check(replace_word("old == 3", "old", "[address]", &found) == "[address] == 3" && found, "a leading word");
    check(replace_word("value == OLD", "old", "X", nullptr) == "value == X", "any case");
    check(replace_word("bold + gold + old1 + old_ + (old)", "old", "X", nullptr) == "bold + gold + old1 + old_ + (X)",
          "only whole words");
    found = true;
    replace_word("a + b", "old", "X", &found);
    check(found, "found is left alone when there is nothing to replace");
    check(replace_word("", "old", "X", nullptr).empty(), "empty text");
    check(replace_word("old old", "old", "1", nullptr) == "1 1", "every occurrence");
  }

  // ---- translate_condition ----
  {
    bool ok = false;
    check(translated(NESDEBUG_SPACE_CPU, "value != old", &ok) == "value != [address]" && ok, "old becomes [address]");
    check(translated(NESDEBUG_SPACE_CPU, "", &ok).empty() && ok, "an empty condition stays empty");
    check(translated(NESDEBUG_SPACE_CPU, "a == 1", &ok) == "a == 1" && ok, "no old, no change");
    translated(NESDEBUG_SPACE_PPU, "old == 0", &ok);
    check(!ok, "old is refused in the PPU space");
    check(translated(NESDEBUG_SPACE_OAM, "old", &ok).find("old") != std::string::npos && !ok, "and says why");
    check(translated(NESDEBUG_SPACE_PALETTE, "value == $21", &ok) == "value == $21" && ok, "value is fine anywhere");
  }

  // ---- syntax_ok: what is fine ----
  check(fine(""), "empty");
  check(fine("a == 5"), "a comparison");
  check(fine("value >= 3 && old == value - 1"), "a condition with value and old");
  check(fine("[$0300] == $FF || {$FFFC} != 0"), "memory reads");
  check(fine("(a + x) * 2 > y"), "parentheses");
  check(fine("-1 < a"), "a leading unary minus");
  check(fine("!(a == 1) && ~x != 0"), "! and ~");
  check(fine("a<<2==x>>1"), "shifts without spaces");
  check(fine("label_1+3"), "labels");
  check(fine("pc >= $C000 && pc <= $C0FF"), "a range test");
  check(fine("[[$10] + 1]"), "nested brackets");
  check(fine("a - -1"), "a unary minus after a binary one");

  // ---- syntax_ok: what is refused ----
  check(!fine("value =="), "a trailing operator");
  check(!fine("== 3"), "a leading binary operator");
  check(!fine("(a"), "an unclosed (");
  check(!fine("a)"), "a stray )");
  check(!fine("[a)"), "mismatched brackets");
  check(!fine("a b"), "two operands");
  check(!fine("a ="), "a lone =");
  check(!fine("a == == 3"), "two operators in a row");
  check(!fine("a == ()"), "an empty group");
  check(!fine("a (1)"), "a call with no operator");
  check(!fine("a #"), "a foreign character");
  check(!fine("a &&"), "a trailing &&");
  check(!fine("[]"), "empty brackets");
  check(!fine("this bogus (("), "the old test's nonsense");
  {
    std::string why;
    syntax_ok("value ==", why);
    check(why.find("operator") != std::string::npos, "the reason is in words");
  }

  // ---- fuzz: garbage in, an answer out ----
  {
    srand(12345);
    static const char alphabet[] = "aAx01_$@.()[]{} +-*/%<>=!&|^~'\t\"#:,;\\?";
    int answered = 0;
    for(int i = 0; i < 20000; i++)
    {
      std::string text;
      const int len = rand() % 24;
      for(int k = 0; k < len; k++) text += alphabet[rand() % (int)(sizeof alphabet - 1)];
      std::string why, out, err;
      syntax_ok(text, why);
      translate_condition(rand() % 4, text, out, err);
      replace_word(text, "old", "[address]", nullptr);
      answered++;
    }
    check(answered == 20000, "20000 random strings answered");
  }

  printf("%d failure(s)\n", failures);
  return failures ? 1 : 0;
}
