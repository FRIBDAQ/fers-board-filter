// Tests of BoardRouter with ring items built exactly as janus-frib's
// RingBufferHandler::writeToRing builds them.  Needs only DataFormat.h.
//
//   test_router                 run the checks
//   test_router --write FILE    also write the test items as an event file
//                               (a file:// source for fersBoardFilter)

#include "BoardRouter.h"

#include <DataFormat.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace std;
typedef vector<uint8_t> Bytes;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { failures++; \
  cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond << endl; } } while (0)

template <typename T> static void put(Bytes& b, const T& v)
{
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + sizeof v);
}
template <typename T> static T get(const Bytes& b, size_t off)
{
  T v; memcpy(&v, b.data() + off, sizeof v); return v;
}

// Ring item with a body header (RingBufferHandler always writes one)
static Bytes ringItem(uint32_t type, uint64_t ts, uint32_t sid, uint32_t barrier, const Bytes& body)
{
  Bytes b;
  RingItemHeader h; h.s_size = sizeof h + sizeof(BodyHeader) + body.size(); h.s_type = type;
  BodyHeader bh; bh.s_size = sizeof bh; bh.s_timestamp = ts; bh.s_sourceId = sid; bh.s_barrier = barrier;
  put(b, h); put(b, bh); b.insert(b.end(), body.begin(), body.end());
  return b;
}
// Ring item without a body header (12.x marker: uint32 == 4)
static Bytes bareItem(uint32_t type, const Bytes& body)
{
  Bytes b;
  RingItemHeader h; h.s_size = sizeof h + sizeof(uint32_t) + body.size(); h.s_type = type;
  put(b, h); put(b, uint32_t(sizeof(uint32_t))); b.insert(b.end(), body.begin(), body.end());
  return b;
}

// Janus body: inclusiveSize [metadata] record 0xffffffff pad(0xff)
static Bytes janusBody(const Bytes& record, bool isHeader, uint16_t metadata)
{
  uint16_t incl = 2 + (isHeader ? 0 : 2) + record.size() + 4;
  uint16_t pad = incl % 4 ? 4 - incl % 4 : 0;
  incl += pad;
  Bytes b; put(b, incl);
  if (!isHeader) put(b, metadata);
  b.insert(b.end(), record.begin(), record.end());
  put(b, uint32_t(0xffffffff));
  for (int i = 0; i < pad; i++) b.push_back(0xff);
  return b;
}

// Janus 5.0.1 file header record (FileHeader_t, 25 bytes packed)
static Bytes headerItem(uint8_t acqmode)
{
  Bytes r;
  put(r, uint8_t(3)); put(r, uint8_t(4));                 // dataformat 3.4
  put(r, uint8_t(5)); put(r, uint8_t(0)); put(r, uint8_t(1)); // software 5.0.1
  put(r, uint16_t(5202)); put(r, uint16_t(361));          // boardtype, run
  put(r, acqmode); put(r, uint16_t(4096)); put(r, uint8_t(1));
  put(r, float(0.5)); put(r, uint64_t(1700000000000ULL));
  Bytes body = janusBody(r, true, 0);
  if (body.size() != 32) { cerr << "header record is " << body.size() << " bytes\n"; failures++; }
  return ringItem(PHYSICS_EVENT, 0, 7, 0, body);
}

struct Ev {
  uint8_t  acqmode = 0x01;     // SPECT
  bool     second = false;     // Enable_2nd_tstamp
  bool     deltaTref = false;  // spectiming, format 3.4
  uint8_t  timeunit = 1;
  uint8_t  board = 0;
  double   tstamp_us = 1234.5;
  uint64_t trigger = 42;
  unsigned nhits = 2;
};
static Bytes eventItem(const Ev& e)
{
  Bytes r;
  put(r, uint16_t(0));                       // eventSize: not checked by the router
  put(r, e.board);
  put(r, e.tstamp_us);
  if (e.second) put(r, e.tstamp_us + 1.0);
  if (e.deltaTref) put(r, 0.25);
  if (e.acqmode != 0x02) {
    put(r, e.trigger); put(r, uint64_t(0x3)); put(r, uint16_t(e.nhits));
    for (unsigned i = 0; i < e.nhits; i++) { put(r, uint8_t(i)); put(r, uint8_t(0x03)); put(r, uint16_t(100+i)); put(r, uint16_t(200+i)); }
  } else {
    put(r, uint16_t(e.nhits));
    for (unsigned i = 0; i < e.nhits; i++) { put(r, uint8_t(i)); put(r, uint8_t(0x30)); put(r, uint32_t(10+i)); put(r, uint16_t(3)); }
  }
  uint16_t metadata = (uint16_t(e.acqmode) << 8) | e.timeunit | (e.second ? 0x8000 : 0);
  return ringItem(PHYSICS_EVENT, uint64_t(e.tstamp_us*10000), 7, 0, janusBody(r, false, metadata));
}
static Bytes stateItem(uint32_t type, bool bodyHeader)
{
  Bytes body;
  put(body, uint32_t(361)); put(body, uint32_t(0)); put(body, uint32_t(1700000000)); put(body, uint32_t(1)); put(body, uint32_t(7));
  char title[81] = "test run"; body.insert(body.end(), title, title + sizeof title);
  return bodyHeader ? ringItem(type, 1700000000, 7, type == BEGIN_RUN ? 1 : 2, body) : bareItem(type, body);
}

static const size_t SID = sizeof(RingItemHeader) + 12;   // s_sourceId offset
static const size_t TS  = sizeof(RingItemHeader) + 4;    // s_timestamp offset

// Equal except the body-header source id (and timestamp when ignoreTs)
static bool sameButHeader(const Bytes& a, const Bytes& b, bool ignoreTs)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (i >= SID && i < SID + 4) continue;
    if (ignoreTs && i >= TS && i < TS + 8) continue;
    if (a[i] != b[i]) return false;
  }
  return true;
}

int main(int argc, char** argv)
{
  vector<Bytes> stream;                     // what Janus would write for one run
  stream.push_back(stateItem(BEGIN_RUN, true));
  stream.push_back(headerItem(0x01));
  for (int t = 0; t < 5; t++) {
    for (uint8_t b : {uint8_t(0), uint8_t(1), uint8_t(2)}) {    // board 2 is not active
      Ev e; e.board = b; e.trigger = t; e.tstamp_us = 1000.0*t + b;
      stream.push_back(eventItem(e));
    }
  }
  stream.push_back(stateItem(END_RUN, true));

  // --- routing with the Janus timestamps kept, boards 0 and 1 active
  {
    BoardRouter r(0x3, BoardRouter::Timestamp::Keep);
    CHECK(r.dataFormat() == 0);
    size_t n[8] = {}; size_t bcast = 0;
    for (auto& item : stream) {
      auto outs = r.route(item.data(), item.size());
      uint32_t type = get<uint32_t>(item, 4);
      if (type != PHYSICS_EVENT) {                        // state change: every board, NULL_TIMESTAMP
        CHECK(outs.size() == 2);
        bcast++;
        for (auto& o : outs) {
          CHECK(o.board == 0 || o.board == 1);
          CHECK(get<uint32_t>(o.item, SID) == uint32_t(o.board));
          CHECK(get<uint64_t>(o.item, TS) == UINT64_MAX);
          CHECK(get<uint32_t>(o.item, SID + 4) == (type == BEGIN_RUN ? 1u : 2u));   // barrier kept
          CHECK(sameButHeader(o.item, item, true));
        }
      } else if (item.size() == 8 + 20 + 32) {            // header record: lowest board only, stamp kept
        CHECK(outs.size() == 1);
        if (outs.size() == 1) {
          CHECK(outs[0].board == 0);
          CHECK(get<uint32_t>(outs[0].item, SID) == 0);
          CHECK(get<uint64_t>(outs[0].item, TS) == 0);
          CHECK(sameButHeader(outs[0].item, item, false));
        }
        CHECK(r.dataFormat() == 34);
      } else {
        uint8_t board = item[8 + 20 + 6];
        if (board == 2) { CHECK(outs.empty()); continue; }
        CHECK(outs.size() == 1);
        CHECK(outs[0].board == board); n[board]++;
        CHECK(get<uint32_t>(outs[0].item, SID) == board);
        CHECK(get<uint64_t>(outs[0].item, TS) == get<uint64_t>(item, TS));
        CHECK(sameButHeader(outs[0].item, item, false));
      }
    }
    CHECK(n[0] == 5 && n[1] == 5);
    CHECK(bcast == 2);
    CHECK(r.stats().perBoard[0] == 5 && r.stats().perBoard[1] == 5 && r.stats().perBoard[2] == 0);
    CHECK(r.stats().broadcast == 2);
    CHECK(r.stats().headerRecords == 1);
    CHECK(r.stats().maskedDropped == 5);
    CHECK(r.stats().badDropped == 0);
    CHECK(r.dataFormat() == 34);
    auto msgs = r.takeMessages();
    CHECK(msgs.size() == 1 && msgs[0].find("ignoring board 2") != string::npos);
    CHECK(r.takeMessages().empty());
  }

  // --- the header record goes to the lowest *active* board
  {
    BoardRouter r(0x6, BoardRouter::Timestamp::Keep);
    Bytes hdr = headerItem(0x01);
    auto outs = r.route(hdr.data(), hdr.size());
    CHECK(outs.size() == 1 && outs[0].board == 1 && get<uint32_t>(outs[0].item, SID) == 1);
  }

  // --- trigger_id as timestamp, in every record layout
  {
    BoardRouter r(0xff, BoardRouter::Timestamp::Trigger);
    // spectiming before any header record: the DeltaTref question is open,
    // so the Janus stamp is kept and said once
    {
      Ev e; e.acqmode = 0x03; e.deltaTref = true; e.board = 2; e.tstamp_us = 5.0; e.trigger = 7;
      Bytes item = eventItem(e);
      auto outs = r.route(item.data(), item.size());
      CHECK(outs.size() == 1 && get<uint64_t>(outs[0].item, TS) == 50000);
      auto msgs = r.takeMessages();
      CHECK(msgs.size() == 1 && msgs[0].find("no Janus header record") != string::npos);
      // spect events do not depend on the format: trigger used right away
      Ev s; s.board = 2; s.trigger = 8;
      Bytes sitem = eventItem(s);
      outs = r.route(sitem.data(), sitem.size());
      CHECK(outs.size() == 1 && get<uint64_t>(outs[0].item, TS) == 8);
    }
    Bytes hdr = headerItem(0x03); r.route(hdr.data(), hdr.size());
    struct { Ev e; const char* what; } cases[] = {
      {Ev(), "spect"},
      {[]{ Ev e; e.second = true; return e; }(), "spect + 2nd tstamp"},
      {[]{ Ev e; e.acqmode = 0x03; e.deltaTref = true; return e; }(), "spectiming 3.4 (DeltaTref)"},
      {[]{ Ev e; e.acqmode = 0x03; e.deltaTref = true; e.second = true; return e; }(), "spectiming + 2nd tstamp"},
      {[]{ Ev e; e.acqmode = 0x04; e.nhits = 0; return e; }(), "counting"},
    };
    for (auto& c : cases) {
      c.e.trigger = 0x0102030405060708ULL; c.e.board = 3;
      Bytes item = eventItem(c.e);
      auto outs = r.route(item.data(), item.size());
      CHECK(outs.size() == 1);
      if (outs.size() == 1) {
        if (get<uint64_t>(outs[0].item, TS) != c.e.trigger) { failures++; cerr << "FAIL trigger timestamp: " << c.what << endl; }
        CHECK(get<uint32_t>(outs[0].item, SID) == 3);
        CHECK(sameButHeader(outs[0].item, item, true));
      }
    }
    // format 3.3: spectiming has no DeltaTref; the trigger follows the timestamps
    {
      BoardRouter r33(0xff, BoardRouter::Timestamp::Trigger);
      Bytes h = headerItem(0x03); h[8 + 20 + 3] = 3;      // minor = 3
      r33.route(h.data(), h.size()); CHECK(r33.dataFormat() == 33);
      Ev e; e.acqmode = 0x03; e.deltaTref = false; e.trigger = 99;
      Bytes item = eventItem(e);
      auto outs = r33.route(item.data(), item.size());
      CHECK(outs.size() == 1 && get<uint64_t>(outs[0].item, TS) == 99);
    }
    // timing mode: no trigger_id, timestamp kept, one warning
    {
      Ev e; e.acqmode = 0x02; e.board = 1; e.tstamp_us = 77.0;
      Bytes item = eventItem(e);
      auto outs = r.route(item.data(), item.size());
      CHECK(outs.size() == 1 && get<uint64_t>(outs[0].item, TS) == 770000);
      auto msgs = r.takeMessages();
      CHECK(msgs.size() == 1 && msgs[0].find("timing-mode") != string::npos);
    }
    // a timing-mode event with one hit is 32 bytes like the header record;
    // the time unit byte tells them apart
    {
      Ev e; e.acqmode = 0x02; e.board = 1; e.nhits = 1;
      Bytes item = eventItem(e);
      CHECK(item.size() == 8 + 20 + 32);
      auto outs = r.route(item.data(), item.size());
      CHECK(outs.size() == 1 && outs[0].board == 1);
    }
  }

  // --- items without a body header get one with the board id and a barrier
  {
    BoardRouter r(0x5, BoardRouter::Timestamp::Keep);
    Bytes begin = stateItem(BEGIN_RUN, false);
    auto outs = r.route(begin.data(), begin.size());
    CHECK(outs.size() == 2);
    for (auto& o : outs) {
      CHECK(o.item.size() == begin.size() - 4 + 20);
      CHECK(get<uint32_t>(o.item, 0) == o.item.size());
      CHECK(get<uint32_t>(o.item, 8) == 20);
      CHECK(get<uint64_t>(o.item, TS) == UINT64_MAX);
      CHECK(get<uint32_t>(o.item, SID) == uint32_t(o.board));
      CHECK(get<uint32_t>(o.item, SID + 4) == 1);
      CHECK(equal(begin.begin() + 12, begin.end(), o.item.begin() + 28));
    }
    Bytes end = stateItem(END_RUN, false);
    outs = r.route(end.data(), end.size());
    CHECK(outs.size() == 2 && get<uint32_t>(outs[0].item, SID + 4) == 2);
  }

  // --- body header with an extension: the body is found after s_size bytes
  {
    BoardRouter r(0xff, BoardRouter::Timestamp::Trigger);
    Ev e; e.board = 4; e.trigger = 5;
    Bytes item = eventItem(e);
    Bytes ext = item; ext.insert(ext.begin() + 28, {0xaa, 0xbb, 0xcc, 0xdd});
    uint32_t size = ext.size(); memcpy(ext.data(), &size, 4);
    uint32_t bhs = 24; memcpy(ext.data() + 8, &bhs, 4);
    auto outs = r.route(ext.data(), ext.size());
    CHECK(outs.size() == 1 && outs[0].board == 4);
    if (outs.size() == 1) {
      CHECK(get<uint64_t>(outs[0].item, TS) == 5);
      CHECK(outs[0].item.size() == ext.size());
      CHECK(outs[0].item[28] == 0xaa && outs[0].item[31] == 0xdd);
    }
  }

  // --- bad input is dropped, not propagated
  {
    BoardRouter r(0xffff, BoardRouter::Timestamp::Keep);
    Ev e; e.board = 20; Bytes big = eventItem(e);
    CHECK(r.route(big.data(), big.size()).empty());
    Bytes trunc(big.begin(), big.begin() + 30);               // size field disagrees
    CHECK(r.route(trunc.data(), trunc.size()).empty());
    Bytes tiny = ringItem(PHYSICS_EVENT, 0, 7, 0, Bytes{1, 2});
    CHECK(r.route(tiny.data(), tiny.size()).empty());
    CHECK(r.stats().badDropped == 3);
    CHECK(r.takeMessages().size() == 3);
    Bytes ok = eventItem(Ev());
    CHECK(r.route(ok.data(), ok.size()).size() == 1);
  }

  if (argc == 3 && string(argv[1]) == "--write") {
    FILE* f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    for (auto& item : stream) fwrite(item.data(), 1, item.size(), f);
    fclose(f);
    cout << "wrote " << stream.size() << " items to " << argv[2] << endl;
  }

  if (failures) { cerr << failures << " check(s) failed" << endl; return 1; }
  cout << "test_router: all checks passed" << endl;
  return 0;
}
