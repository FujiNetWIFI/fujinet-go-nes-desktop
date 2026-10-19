/*
 * fuji_trace -- see fuji_trace.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fuji_trace.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "fuji_names.h"

namespace {

const char* fuji_command_name(int id)
{
  for(const auto& c : fuji_command_names)
    if(c.id == id) return c.name;
  return "";
}

// "NES -> FujiNet 70 F4 READ HOST SLOTS (6 bytes) x3"
void describe(nesdebug_fuji_packet& p)
{
  const char* way = p.to_host ? "FujiNet \xe2\x86\x92 NES" : "NES \xe2\x86\x92 FujiNet";
  char times[16] = "";
  if(p.repeat > 1) snprintf(times, sizeof times, " \xc3\x97%u", p.repeat);
  if(p.device >= 0)
  {
    const char* name = fuji_command_name(p.command);
    snprintf(p.text, sizeof p.text, "%s %02X %02X%s%s (%d bytes)%s", way, p.device, p.command,
             name[0] ? " " : "", name, p.nbytes, times);
  }
  else
    snprintf(p.text, sizeof p.text, "%s %d bytes%s", way, p.nbytes, times);
}

} // namespace

void FujiTrace::Feed(int dir, const uint8_t* buf, size_t len)
{
  std::lock_guard<std::mutex> lock(mutex);
  const int toHost = dir == 0 ? 1 : 0;
  // A send throws away a half-read reply: what comes next starts afresh.
  if(!toHost)
  {
    slip[1].clear();
    esc[1] = false;
  }
  std::vector<uint8_t>& acc = slip[toHost];
  for(size_t i = 0; i < len; i++)
  {
    const uint8_t b = buf[i];
    if(b == 0xC0)
    {
      if(!acc.empty())
      {
        Emit(toHost, acc);
        acc.clear();
      }
      esc[toHost] = false;
      continue;
    }
    if(esc[toHost])
    {
      acc.push_back(b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b);
      esc[toHost] = false;
    }
    else if(b == 0xDB)
      esc[toHost] = true;
    else if(acc.size() < 4096)
      acc.push_back(b);
  }
}

// A merged entry gets a new serial, so a window that rebuilds its log on a
// changed last serial sees the count go up.
void FujiTrace::Restamp(Entry& e)
{
  e.p.serial = ++serial;
  describe(e.p);
}

void FujiTrace::Emit(int toHost, const std::vector<uint8_t>& acc)
{
  ++total;
  Entry e;
  memset(&e.p, 0, sizeof e.p);
  e.bytes = acc;
  e.p.to_host = toHost;
  e.p.repeat = 1;
  e.p.nbytes = (int)acc.size();
  memcpy(e.p.data, acc.data(), std::min(acc.size(), sizeof e.p.data));
  e.p.device = acc.size() >= 6 ? acc[0] : -1;
  e.p.command = acc.size() >= 6 ? acc[1] : -1;
  e.p.length = acc.size() >= 6 ? (acc[2] | (acc[3] << 8)) : -1;
  Restamp(e);
  ring.push_back(std::move(e));

  // A byte-identical repeat folds into the entries before it, which count
  // it: one packet again, or an exchange (a request, then its reply) again.
  auto same = [](const Entry& a, const Entry& b) {
    return a.p.to_host == b.p.to_host && a.bytes == b.bytes;
  };
  const size_t n = ring.size();
  if(n >= 2 && same(ring[n - 2], ring[n - 1]) && !ring[n - 2].paired)
  {
    ring[n - 2].p.repeat += ring[n - 1].p.repeat;
    ring.pop_back();
    Restamp(ring.back());
  }
  else if(n >= 4 && same(ring[n - 4], ring[n - 2]) && same(ring[n - 3], ring[n - 1]) &&
          !ring[n - 4].p.to_host && ring[n - 3].p.to_host &&
          ring[n - 4].p.repeat == ring[n - 3].p.repeat &&
          ring[n - 2].p.repeat == 1 && ring[n - 1].p.repeat == 1)
  {
    ring.pop_back();
    ring.pop_back();
    ring[n - 4].p.repeat++;
    ring[n - 3].p.repeat++;
    ring[n - 4].paired = ring[n - 3].paired = true;
    Restamp(ring[n - 4]);
    Restamp(ring[n - 3]);
  }
  if(ring.size() > Max) ring.pop_front();
}

int FujiTrace::Copy(nesdebug_fuji_packet* out, int max)
{
  std::lock_guard<std::mutex> lock(mutex);
  if(!out || max <= 0) return 0;
  int n = 0;
  const size_t start = ring.size() > (size_t)max ? ring.size() - (size_t)max : 0;
  for(size_t i = start; i < ring.size(); i++)
    out[n++] = ring[i].p;
  return n;
}

void FujiTrace::Clear()
{
  std::lock_guard<std::mutex> lock(mutex);
  ring.clear();
}

uint64_t FujiTrace::Total()
{
  std::lock_guard<std::mutex> lock(mutex);
  return total;
}
