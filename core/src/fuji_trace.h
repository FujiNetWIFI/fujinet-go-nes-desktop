/*
 * fuji_trace -- the FujiNet tab's packet log: the bytes on the cartridge's
 * link to FujiNet, both ways, SLIP-decoded into FujiBus packets, named, and
 * kept in a ring. Fed by the MesenCE fork's FujiNetCart::SetTrace from the
 * cartridge's worker thread; read by the debugger's API on the UI thread.
 *
 * No emulator in here, so core/tests/fujitrace_test.cpp drives it with
 * synthetic bytes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "nesdebug.h"

class FujiTrace
{
public:
  static constexpr size_t Max = 512;

  // dir 0: FujiNet -> NES, as received; dir 1: NES -> FujiNet, as sent.
  void Feed(int dir, const uint8_t* buf, size_t len);
  // The newest max packets, oldest first. Returns the count.
  int Copy(nesdebug_fuji_packet* out, int max);
  void Clear();
  // Packets decoded since the start, repeats and cleared ones included.
  uint64_t Total();

private:
  struct Entry
  {
    nesdebug_fuji_packet p;
    std::vector<uint8_t> bytes;   // the whole decoded packet, for repeats
    bool paired{false};           // counts an exchange with its neighbour
  };

  void Emit(int toHost, const std::vector<uint8_t>& acc);
  void Restamp(Entry& e);

  std::mutex mutex;              // guards everything below
  std::deque<Entry> ring;
  uint64_t serial{0};
  uint64_t total{0};
  std::vector<uint8_t> slip[2];  // by to_host
  bool esc[2]{false, false};
};
