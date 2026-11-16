/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions
 * (core/include/nesmemtools.h). Only the public engine API is used, so the
 * pure parts run with no machine.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "nesdebug.h"
#include "nesmemtools.h"
}

namespace {

std::string trim(const std::string& s)
{
  size_t a = 0, b = s.size();
  while(a < b && isspace((unsigned char)s[a])) a++;
  while(b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

bool all_of(const std::string& s, int (*pred)(int))
{
  if(s.empty()) return false;
  for(char c : s)
    if(!pred((unsigned char)c)) return false;
  return true;
}

bool is_label_name(const std::string& s)
{
  if(s.empty() || isdigit((unsigned char)s[0])) return false;
  for(char c : s)
    if(!isalnum((unsigned char)c) && c != '_' && c != '@' && c != '.') return false;
  return true;
}

// One address token: $C000, 0xC000, C000 (1-4 hex digits, unless a label),
// a label, or an expression.
bool parse_value(nesdebug* d, const std::string& raw, long& out)
{
  const std::string t = trim(raw);
  if(t.empty()) return false;
  char* end = nullptr;
  if(t[0] == '$' && t.size() > 1 && all_of(t.substr(1), isxdigit))
  {
    out = strtol(t.c_str() + 1, &end, 16);
    return true;
  }
  if(t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X') && all_of(t.substr(2), isxdigit))
  {
    out = strtol(t.c_str() + 2, &end, 16);
    return true;
  }
  if(is_label_name(t) && d)
  {
    const int a = nesdebug_label_address(d, t.c_str());
    if(a >= 0) { out = a; return true; }
  }
  if(t.size() <= 4 && all_of(t, isxdigit))
  {
    out = strtol(t.c_str(), &end, 16);
    return true;
  }
  if(d)
  {
    char err[96];
    long v = 0;
    if(nesdebug_eval(d, t.c_str(), &v, err, sizeof err) == 0) { out = v; return true; }
  }
  return false;
}

bool plain_token(const std::string& s)
{
  const std::string t = trim(s);
  if(t.empty()) return false;
  if(t[0] == '$') return t.size() > 1 && all_of(t.substr(1), isxdigit);
  if(t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) return all_of(t.substr(2), isxdigit);
  if(all_of(t, isxdigit)) return true;
  return is_label_name(t);
}

// ---- the watch list ----

std::mutex gWatchLock;
std::vector<std::string> gWatches;
unsigned gWatchGen = 1;

} // namespace

extern "C" int nessym_parse_range(nesdebug* d, const char* text, uint16_t* low, uint16_t* high)
{
  if(!text) return 0;
  const std::string t = trim(text);
  if(t.empty()) return 0;
  long a = 0, b = 0;
  bool have = false;
  size_t cut = t.find("..");
  size_t skip = 2;
  if(cut == std::string::npos)
  {
    // "low-high" only when both sides are plain tokens ("a-b" is also a subtraction)
    cut = t.find('-');
    skip = 1;
    if(cut != std::string::npos && !(plain_token(t.substr(0, cut)) && plain_token(t.substr(cut + 1))))
      cut = std::string::npos;
  }
  if(cut != std::string::npos && cut > 0)
  {
    have = parse_value(d, t.substr(0, cut), a) && parse_value(d, t.substr(cut + skip), b);
  }
  else
  {
    have = parse_value(d, t, a);
    b = a;
  }
  if(!have || a < 0 || b < 0 || a > 0xFFFF || b > 0xFFFF) return 0;
  if(a > b) { const long tmp = a; a = b; b = tmp; }
  if(low) *low = (uint16_t)a;
  if(high) *high = (uint16_t)b;
  return 1;
}

extern "C" int neswatch_count(void)
{
  std::lock_guard<std::mutex> lock(gWatchLock);
  return (int)gWatches.size();
}

extern "C" int neswatch_add(const char* text)
{
  const std::string t = text ? trim(text) : "";
  if(t.empty()) return -1;
  std::lock_guard<std::mutex> lock(gWatchLock);
  if((int)gWatches.size() >= NESWATCH_MAX) return -1;
  gWatches.push_back(t);
  ++gWatchGen;
  return (int)gWatches.size() - 1;
}

extern "C" void neswatch_remove(int index)
{
  std::lock_guard<std::mutex> lock(gWatchLock);
  if(index < 0 || index >= (int)gWatches.size()) return;
  gWatches.erase(gWatches.begin() + index);
  ++gWatchGen;
}

extern "C" void neswatch_clear(void)
{
  std::lock_guard<std::mutex> lock(gWatchLock);
  gWatches.clear();
  ++gWatchGen;
}

extern "C" const char* neswatch_text(int index)
{
  static thread_local std::string copy;
  std::lock_guard<std::mutex> lock(gWatchLock);
  if(index < 0 || index >= (int)gWatches.size()) return "";
  copy = gWatches[(size_t)index];
  return copy.c_str();
}

extern "C" unsigned neswatch_generation(void)
{
  std::lock_guard<std::mutex> lock(gWatchLock);
  return gWatchGen;
}

extern "C" int neswatch_range(nesdebug* d, int index, uint16_t* low, uint16_t* high)
{
  std::string t;
  {
    std::lock_guard<std::mutex> lock(gWatchLock);
    if(index < 0 || index >= (int)gWatches.size()) return 0;
    t = gWatches[(size_t)index];
  }
  return nessym_parse_range(d, t.c_str(), low, high);
}

extern "C" int neswatch_format(nesdebug* d, int index, char* dst, int dstsz)
{
  if(!dst || dstsz <= 0) return 0;
  std::string t;
  {
    std::lock_guard<std::mutex> lock(gWatchLock);
    if(index < 0 || index >= (int)gWatches.size()) { dst[0] = '\0'; return 0; }
    t = gWatches[(size_t)index];
  }
  uint16_t lo = 0, hi = 0;
  char buf[256];
  if(!nessym_parse_range(d, t.c_str(), &lo, &hi))
    return snprintf(dst, (size_t)dstsz, "%s  ?", t.c_str());
  const bool running = !d || !nesdebug_is_stopped(d);
  if(lo == hi)
  {
    if(running)
      snprintf(buf, sizeof buf, "%s  $%04X  -", t.c_str(), lo);
    else
    {
      uint8_t v = 0;
      nesdebug_read(d, lo, &v, 1);
      snprintf(buf, sizeof buf, "%s  $%04X  $%02X (%u)", t.c_str(), lo, v, v);
    }
    return snprintf(dst, (size_t)dstsz, "%s", buf);
  }
  std::string s = t + "  $";
  char head[32];
  snprintf(head, sizeof head, "%04X-$%04X  ", lo, hi);
  s += head;
  if(running)
    s += "-";
  else
  {
    uint8_t bytes[8];
    const int n = (int)std::min<unsigned>(8, (unsigned)(hi - lo + 1));
    nesdebug_read(d, lo, bytes, n);
    for(int i = 0; i < n; i++)
    {
      char h[8];
      snprintf(h, sizeof h, "%s%02X", i ? " " : "", bytes[i]);
      s += h;
    }
    if(hi - lo + 1 > 8) s += " ...";
  }
  return snprintf(dst, (size_t)dstsz, "%s", s.c_str());
}

extern "C" int nessearch_parse(int kind, const char* text, uint8_t* pat, int* count, uint8_t* mask, int* flags)
{
  if(!text || !pat || !count) return 0;
  int n = 0;
  if(kind == NESSEARCH_TEXT)
  {
    const size_t len = strlen(text);
    if(!len || len > NESSEARCH_MAX) return 0;
    for(size_t i = 0; i < len; i++) pat[n++] = (uint8_t)text[i];
    if(flags) *flags = NESDEBUG_SEARCH_NOCASE;
  }
  else
  {
    std::string hexd;
    const char* p = text;
    while(*p)
    {
      if(isspace((unsigned char)*p) || *p == ',') { p++; continue; }
      if(*p == '$') { p++; continue; }
      if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; continue; }
      if(!isxdigit((unsigned char)*p)) return 0;
      hexd += *p++;
    }
    if(hexd.empty() || (hexd.size() & 1) || hexd.size() / 2 > NESSEARCH_MAX) return 0;
    for(size_t i = 0; i < hexd.size(); i += 2)
      pat[n++] = (uint8_t)strtol(hexd.substr(i, 2).c_str(), nullptr, 16);
    if(flags) *flags = 0;
  }
  *count = n;
  if(mask) *mask = 0xFF;
  return 1;
}

extern "C" int nesbp_describe(nesdebug* d, const nesdebug_breakpoint* bpt, char* dst, int dstsz)
{
  if(!bpt || !dst || dstsz <= 0) return 0;
  const char* kind = (bpt->type & NESDEBUG_BP_EXEC) ? "Execute"
                   : (bpt->type & NESDEBUG_BP_ACCESS) == NESDEBUG_BP_ACCESS ? "Access"
                   : (bpt->type & NESDEBUG_BP_READ) ? "Read" : "Write";
  std::string s = kind;
  s += "  ";
  const char* space = bpt->space == NESDEBUG_SPACE_PPU ? "ppu "
                    : bpt->space == NESDEBUG_SPACE_PALETTE ? "palette "
                    : bpt->space == NESDEBUG_SPACE_OAM ? "oam " : "";
  const int digits = (bpt->space == NESDEBUG_SPACE_PALETTE || bpt->space == NESDEBUG_SPACE_OAM) ? 2 : 4;
  char a[48];
  s += space;
  snprintf(a, sizeof a, "$%0*X", digits, bpt->start);
  s += a;
  if(bpt->end != bpt->start)
  {
    snprintf(a, sizeof a, "-$%0*X", digits, bpt->end);
    s += a;
  }
  if(bpt->space == NESDEBUG_SPACE_CPU && d)
  {
    char label[64];
    if(nesdebug_address_label(d, bpt->start, label, sizeof label) > 0)
      s += std::string(" (") + label + ")";
  }
  if(bpt->condition[0]) s += std::string("  if ") + bpt->condition;
  return snprintf(dst, (size_t)dstsz, "%s", s.c_str());
}

extern "C" int nesmem_region_name(const nesdebug_region* map, int count, uint16_t addr, char* dst, int dstsz)
{
  if(!dst || dstsz <= 0) return 0;
  dst[0] = '\0';
  for(int i = 0; map && i < count; i++)
    if(addr >= map[i].low && addr <= map[i].high)
      return snprintf(dst, (size_t)dstsz, "%s", map[i].name);
  return 0;
}
