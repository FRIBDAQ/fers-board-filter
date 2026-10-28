// Tests of EventBuilder with ring items built exactly as janus-frib's
// RingBufferHandler::writeToRing builds them.  Needs only DataFormat.h.
//
//   test_builder                run the checks
//   test_builder --write FILE   also write the test stream as an event file
//                               (a file:// source for fersBoardFilter)

#include "EventBuilder.h"

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
static Bytes headerItem(uint8_t acqmode, uint8_t minor = 4)
{
  Bytes r;
  put(r, uint8_t(3)); put(r, minor);                      // dataformat 3.x
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
static uint64_t stamp(const Ev& e) { return uint64_t(e.tstamp_us*10000 + 0.5); }
static Bytes eventItem(const Ev& e)
{
  Bytes r;
  put(r, uint16_t(0));                       // eventSize: not checked by the builder
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
  return ringItem(PHYSICS_EVENT, stamp(e), 7, 0, janusBody(r, false, metadata));
}
static Bytes stateItem(uint32_t type, bool bodyHeader)
{
  Bytes body;
  put(body, uint32_t(361)); put(body, uint32_t(0)); put(body, uint32_t(1700000000)); put(body, uint32_t(1)); put(body, uint32_t(7));
  char title[81] = "test run"; body.insert(body.end(), title, title + sizeof title);
  return bodyHeader ? ringItem(type, 1700000000, 7, type == BEGIN_RUN ? 1 : 2, body) : bareItem(type, body);
}

static const size_t SID = sizeof(RingItemHeader) + 12;   // s_sourceId offset in an item
static const size_t TS  = sizeof(RingItemHeader) + 4;    // s_timestamp offset in an item
static const size_t BODY = sizeof(RingItemHeader) + sizeof(BodyHeader);
static const size_t FRAG = 20;                           // flat fragment header

// A fragment of a built event
struct Frag { uint64_t ts; uint32_t sid; uint32_t size; uint32_t barrier; Bytes item; };

// Takes a built event apart, checking its layout against glom's
static vector<Frag> fragments(const Bytes& e)
{
  vector<Frag> out;
  CHECK(get<uint32_t>(e, 0) == e.size());
  CHECK(get<uint32_t>(e, 4) == PHYSICS_EVENT);
  CHECK(get<uint32_t>(e, 8) == sizeof(BodyHeader));
  CHECK(get<uint32_t>(e, SID + 4) == 0);                 // barrier
  uint32_t bodyBytes = get<uint32_t>(e, BODY);
  CHECK(bodyBytes == e.size() - BODY);
  size_t p = BODY + 4;
  while (p + FRAG <= e.size()) {
    Frag f;
    f.ts = get<uint64_t>(e, p); f.sid = get<uint32_t>(e, p + 8);
    f.size = get<uint32_t>(e, p + 12); f.barrier = get<uint32_t>(e, p + 16);
    p += FRAG;
    CHECK(p + f.size <= e.size());
    f.item.assign(e.begin() + p, e.begin() + p + f.size);
    CHECK(get<uint32_t>(f.item, 0) == f.size);
    p += f.size;
    out.push_back(f);
  }
  CHECK(p == e.size());
  return out;
}

// Equal except the body-header source id
static bool sameButSid(const Bytes& a, const Bytes& b)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (i >= SID && i < SID + 4) continue;
    if (a[i] != b[i]) return false;
  }
  return true;
}

// The one fragment of a board's item, as the builder should have made it
static void checkFragment(const Frag& f, const Bytes& item, unsigned board)
{
  CHECK(f.ts == get<uint64_t>(item, TS));
  CHECK(f.sid == board);
  CHECK(f.barrier == 0);
  CHECK(f.size == item.size());
  CHECK(get<uint32_t>(f.item, SID) == board);
  CHECK(sameButSid(f.item, item));
}

// The checks are written with a 1 us window in 5.0.1 units (tstamp_us * 10000)
static EventBuilder::Config config(uint32_t boards, EventBuilder::Key key = EventBuilder::Key::Time)
{
  EventBuilder::Config c;
  c.activeBoards = boards;
  c.key = key;
  c.dt = 10000;
  c.sourceId = 100;
  return c;
}

int main(int argc, char** argv)
{
  // What Janus writes for one run: the boards' items arrive in bunches, a
  // board's run of triggers, then the next board's; board 2 is not active
  vector<Bytes> stream;
  vector<Bytes> items[3][5];
  stream.push_back(stateItem(BEGIN_RUN, true));
  stream.push_back(headerItem(0x01));
  for (uint8_t b : {uint8_t(0), uint8_t(1), uint8_t(2)}) {
    for (int t = 0; t < 5; t++) {
      Ev e; e.board = b; e.trigger = t; e.tstamp_us = 1000.0*t + 0.01*b;   // 100 ns apart
      items[b][t].push_back(eventItem(e));
      stream.push_back(items[b][t][0]);
    }
  }
  stream.push_back(stateItem(END_RUN, true));

  // --- building by timestamp, boards 0 and 1 active
  {
    EventBuilder eb(config(0x3));
    CHECK(eb.dataFormat() == 0);
    vector<Bytes> out;
    size_t afterBoard0 = 0;
    for (size_t i = 0; i < stream.size(); i++) {
      auto outs = eb.process(stream[i].data(), stream[i].size());
      out.insert(out.end(), outs.begin(), outs.end());
      if (i == 6) afterBoard0 = out.size();          // all of board 0 in, none of board 1
    }
    CHECK(afterBoard0 == 4);                           // format, BEGIN_RUN, glom info, header record only
    CHECK(eb.open() == 0);
    CHECK(out.size() == 4 + 5 + 1);
    if (out.size() == 10) {
      // the run's file starts with the format item
      CHECK(out[0] == EventBuilder::formatItem());
      // BEGIN/END_RUN pass with the filter's source id, otherwise unchanged
      CHECK(sameButSid(out[1], stream[0]) && get<uint32_t>(out[1], SID) == 100);
      CHECK(sameButSid(out[9], stream.back()) && get<uint32_t>(out[9], SID) == 100);
      // after BEGIN_RUN the glom parameters: dt ticks, building, latest (the default)
      CHECK(out[2].size() == sizeof(GlomParameters));
      if (out[2].size() == sizeof(GlomParameters)) {
        GlomParameters g; memcpy(&g, out[2].data(), sizeof g);
        CHECK(g.s_header.s_size == sizeof g && g.s_header.s_type == EVB_GLOM_INFO);
        CHECK(g.s_empty == sizeof(uint32_t) && g.s_coincidenceTicks == eb.config().dt);
        CHECK(g.s_isBuilding == 1 && g.s_timestampPolicy == GLOM_TIMESTAMP_LAST);
      }
      // the header record: a built event of one fragment, payload source id kept
      auto hf = fragments(out[3]);
      CHECK(hf.size() == 1);
      if (hf.size() == 1) {
        CHECK(hf[0].item == stream[1]);
        CHECK(hf[0].sid == 7 && hf[0].ts == 0 && hf[0].barrier == 0);
      }
      CHECK(get<uint64_t>(out[3], TS) == 0);
      CHECK(get<uint32_t>(out[3], SID) == 100);
      CHECK(eb.dataFormat() == 34);
      for (int t = 0; t < 5; t++) {
        const Bytes& e = out[4 + t];
        auto f = fragments(e);
        CHECK(f.size() == 2);
        if (f.size() != 2) continue;
        checkFragment(f[0], items[0][t][0], 0);                 // earlier stamp first, although
        checkFragment(f[1], items[1][t][0], 1);                 // (bunched) board 1's came later
        CHECK(get<uint64_t>(e, TS) == stamp(Ev{0x01, false, false, 1, 1, 1000.0*t + 0.01, 0, 2}));  // latest: board 1's
        CHECK(get<uint32_t>(e, SID) == 100);
      }
    }
    const EventBuilder::Stats& s = eb.stats();
    CHECK(s.built == 5 && s.incomplete == 0 && s.forced == 0 && s.duplicates == 0);
    CHECK(s.perBoard[0] == 5 && s.perBoard[1] == 5 && s.perBoard[2] == 0);
    CHECK(s.headerRecords == 1 && s.passed == 2);
    CHECK(s.maskedDropped == 5 && s.badDropped == 0);
    auto msgs = eb.takeMessages();
    CHECK(msgs.size() == 1 && msgs[0].find("ignoring board 2") != string::npos);
    CHECK(eb.takeMessages().empty());

    // the counts start over with the next run
    size_t n = 0;
    for (auto& item : stream) n += eb.process(item.data(), item.size()).size();
    CHECK(n == 10);                                   // format and glom info again
    CHECK(s.built == 5 && s.perBoard[0] == 5 && s.perBoard[1] == 5);
    CHECK(s.headerRecords == 1 && s.passed == 2 && s.maskedDropped == 5);
    CHECK(eb.takeMessages().empty());                  // still said once per process
  }

  // --- a trigger a board missed is released once that board has gone past it
  {
    EventBuilder eb(config(0x3));
    Bytes b0[3], b1[3];
    for (int t = 0; t < 3; t++) {
      Ev e; e.tstamp_us = 100.0*t; e.board = 0; b0[t] = eventItem(e);
      e.board = 1; e.tstamp_us += 0.05; b1[t] = eventItem(e);
    }
    for (int t = 0; t < 3; t++) CHECK(eb.process(b0[t].data(), b0[t].size()).empty());
    CHECK(eb.open() == 3);
    auto outs = eb.process(b1[0].data(), b1[0].size());
    CHECK(outs.size() == 1 && eb.open() == 2);
    outs = eb.process(b1[2].data(), b1[2].size());     // board 1 skipped trigger 1
    CHECK(outs.size() == 2 && eb.open() == 0);
    if (outs.size() == 2) {
      auto f1 = fragments(outs[0]); auto f2 = fragments(outs[1]);
      CHECK(f1.size() == 1 && f2.size() == 2);
      if (f1.size() == 1) checkFragment(f1[0], b0[1], 0);
    }
    CHECK(eb.stats().built == 3 && eb.stats().incomplete == 1 && eb.stats().forced == 0);
  }

  // --- a board that stopped sending: the oldest goes out at maxPending
  {
    EventBuilder::Config c = config(0x3);
    c.maxPending = 3;
    EventBuilder eb(c);
    size_t released = 0;
    for (int t = 0; t < 6; t++) {
      Ev e; e.tstamp_us = 100.0*t;
      Bytes item = eventItem(e);
      released += eb.process(item.data(), item.size()).size();
    }
    CHECK(released == 3 && eb.open() == 3);
    auto rest = eb.flush();
    CHECK(rest.size() == 3 && eb.open() == 0);
    CHECK(eb.stats().built == 6 && eb.stats().incomplete == 6 && eb.stats().forced == 6);
    // a state change releases what is open, then passes
    Bytes item = eventItem(Ev());
    eb.process(item.data(), item.size());
    Bytes end = stateItem(END_RUN, true);
    auto outs = eb.process(end.data(), end.size());
    CHECK(outs.size() == 2 && eb.open() == 0);
    CHECK(outs.size() == 2 && sameButSid(outs[1], end) && get<uint32_t>(outs[1], SID) == 100);
  }

  // --- the window: within dt joins (even earlier than the first), beyond opens a new trigger
  {
    EventBuilder::Config c = config(0x3);
    c.dt = 10000;                                      // 1 us
    EventBuilder eb(c);
    Ev e; e.board = 1; e.tstamp_us = 500.0;  Bytes first = eventItem(e);
    e.board = 0; e.tstamp_us = 499.5;        Bytes earlier = eventItem(e);   // 0.5 us before
    e.board = 0; e.tstamp_us = 501.5;        Bytes beyond = eventItem(e);    // 1.5 us after
    e.board = 1; e.tstamp_us = 501.6;        Bytes beyond1 = eventItem(e);
    CHECK(eb.process(first.data(), first.size()).empty());
    auto outs = eb.process(earlier.data(), earlier.size());
    CHECK(outs.size() == 1 && eb.open() == 0);
    if (outs.size() == 1) {
      auto f = fragments(outs[0]);
      CHECK(f.size() == 2);
      CHECK(get<uint64_t>(outs[0], TS) == get<uint64_t>(first, TS));     // latest fragment
    }
    CHECK(eb.process(beyond.data(), beyond.size()).empty());
    outs = eb.process(beyond1.data(), beyond1.size());
    CHECK(outs.size() == 1 && eb.stats().built == 2 && eb.stats().duplicates == 0);
    // the same board twice within the window is counted, not refused
    e.board = 0; e.tstamp_us = 700.0; Bytes d1 = eventItem(e);
    e.tstamp_us = 700.3;              Bytes d2 = eventItem(e);
    e.board = 1; e.tstamp_us = 700.1; Bytes d3 = eventItem(e);
    eb.process(d1.data(), d1.size()); eb.process(d2.data(), d2.size());
    outs = eb.process(d3.data(), d3.size());
    CHECK(outs.size() == 1 && eb.stats().duplicates == 1);
    if (outs.size() == 1) CHECK(fragments(outs[0]).size() == 3);
  }

  // --- building by trigger_id: timestamps far apart, same trigger number
  {
    EventBuilder eb(config(0x3, EventBuilder::Key::Trigger));
    Bytes hdr = headerItem(0x03);                      // spectiming 3.4: DeltaTref present
    eb.process(hdr.data(), hdr.size());
    vector<Bytes> out;
    Bytes b0[3], b1[3];
    for (int t = 0; t < 3; t++) {
      Ev e; e.acqmode = 0x03; e.deltaTref = true; e.trigger = 0x0102030405060700ULL + t;
      e.board = 0; e.tstamp_us = 100.0*t;        b0[t] = eventItem(e);
      e.board = 1; e.tstamp_us = 100.0*t + 50.0; b1[t] = eventItem(e);   // 50 us later
    }
    for (int t = 0; t < 3; t++) { auto o = eb.process(b0[t].data(), b0[t].size()); out.insert(out.end(), o.begin(), o.end()); }
    CHECK(out.empty() && eb.open() == 3);
    for (int t = 0; t < 3; t++) { auto o = eb.process(b1[t].data(), b1[t].size()); out.insert(out.end(), o.begin(), o.end()); }
    CHECK(out.size() == 3 && eb.open() == 0);
    for (size_t t = 0; t < out.size(); t++) {
      auto f = fragments(out[t]);
      CHECK(f.size() == 2);
      if (f.size() == 2) { checkFragment(f[0], b0[t], 0); checkFragment(f[1], b1[t], 1); }
    }
    CHECK(eb.stats().built == 3 && eb.stats().incomplete == 0);
    // every record layout finds the trigger_id: spect, 2nd tstamp, counting; format 3.3 spectiming
    struct { Ev e; uint8_t minor; const char* what; } cases[] = {
      {Ev(), 4, "spect"},
      {[]{ Ev e; e.second = true; return e; }(), 4, "spect + 2nd tstamp"},
      {[]{ Ev e; e.acqmode = 0x03; e.deltaTref = true; e.second = true; return e; }(), 4, "spectiming + 2nd tstamp"},
      {[]{ Ev e; e.acqmode = 0x04; e.nhits = 0; return e; }(), 4, "counting"},
      {[]{ Ev e; e.acqmode = 0x03; e.deltaTref = false; return e; }(), 3, "spectiming 3.3 (no DeltaTref)"},
    };
    for (auto& c : cases) {
      EventBuilder e2(config(0x3, EventBuilder::Key::Trigger));
      Bytes h = headerItem(c.e.acqmode, c.minor); e2.process(h.data(), h.size());
      c.e.trigger = 0x0102030405060708ULL; c.e.board = 0; c.e.tstamp_us = 10.0;
      Bytes i0 = eventItem(c.e);
      c.e.board = 1; c.e.tstamp_us = 90.0;             // far outside any window
      Bytes i1 = eventItem(c.e);
      e2.process(i0.data(), i0.size());
      auto o = e2.process(i1.data(), i1.size());
      if (o.size() != 1 || fragments(o[0]).size() != 2) { failures++; cerr << "FAIL trigger key: " << c.what << endl; }
    }
    // timing mode has no trigger_id: built by timestamp, said once
    {
      EventBuilder e3(config(0x3, EventBuilder::Key::Trigger));
      Ev t; t.acqmode = 0x02; t.board = 0; t.tstamp_us = 77.0; Bytes i0 = eventItem(t);
      t.board = 1; t.tstamp_us = 77.5;                               Bytes i1 = eventItem(t);
      e3.process(i0.data(), i0.size());
      auto o = e3.process(i1.data(), i1.size());
      CHECK(o.size() == 1);
      if (o.size() == 1) CHECK(fragments(o[0]).size() == 2);
      auto msgs = e3.takeMessages();
      CHECK(msgs.size() == 1 && msgs[0].find("timing-mode") != string::npos);
      // a timing-mode event with one hit is 32 bytes like the header record;
      // the time unit byte tells them apart
      t.nhits = 1; Bytes one = eventItem(t);
      CHECK(one.size() == 8 + 20 + 32);
      e3.process(one.data(), one.size());
      CHECK(e3.stats().headerRecords == 0 && e3.stats().perBoard[1] == 2);
    }
    // spectiming before any header record: by timestamp until the next run
    {
      EventBuilder e4(config(0x3, EventBuilder::Key::Trigger));
      Ev s; s.acqmode = 0x03; s.deltaTref = true; s.board = 0; s.tstamp_us = 5.0; s.trigger = 7;
      Bytes i0 = eventItem(s);
      s.board = 1; s.tstamp_us = 5.2; s.trigger = 9;   // different trigger, same time
      Bytes i1 = eventItem(s);
      e4.process(i0.data(), i0.size());
      auto o = e4.process(i1.data(), i1.size());
      CHECK(o.size() == 1);
      auto msgs = e4.takeMessages();
      CHECK(msgs.size() == 1 && msgs[0].find("no Janus header record") != string::npos);
    }
  }

  // --- items without a body header: state changes pass as they are (no
  //     source id to set), events are dropped (Janus always writes one)
  {
    EventBuilder eb(config(0x1));
    Bytes begin = stateItem(BEGIN_RUN, false);
    auto outs = eb.process(begin.data(), begin.size());
    CHECK(outs.size() == 3 && outs[1] == begin);       // format, BEGIN_RUN, glom info
    Bytes bare = bareItem(PHYSICS_EVENT, Bytes(eventItem(Ev()).begin() + BODY, eventItem(Ev()).end()));
    CHECK(eb.process(bare.data(), bare.size()).empty());
    CHECK(eb.stats().badDropped == 1 && eb.stats().passed == 1);
  }

  // --- body header with an extension: the body is found after s_size bytes
  {
    EventBuilder eb(config(0x3));
    Ev e; e.board = 1;
    Bytes item = eventItem(e);
    Bytes ext = item; ext.insert(ext.begin() + 28, {0xaa, 0xbb, 0xcc, 0xdd});
    uint32_t size = ext.size(); memcpy(ext.data(), &size, 4);
    uint32_t bhs = 24; memcpy(ext.data() + 8, &bhs, 4);
    e.board = 0; Bytes other = eventItem(e);
    eb.process(ext.data(), ext.size());
    auto outs = eb.process(other.data(), other.size());
    CHECK(outs.size() == 1);
    if (outs.size() == 1) {
      auto f = fragments(outs[0]);
      CHECK(f.size() == 2);
      if (f.size() == 2) {
        // equal timestamps: fragments in source id order
        CHECK(f[0].sid == 0 && f[1].sid == 1);
        CHECK(f[1].item.size() == ext.size());
        CHECK(f[1].item[28] == 0xaa && f[1].item[31] == 0xdd);
        CHECK(get<uint32_t>(f[1].item, SID) == 1);
      }
    }
  }

  // --- bad input is dropped, not propagated
  {
    EventBuilder eb(config(0xffff));
    Ev e; e.board = 20; Bytes big = eventItem(e);
    CHECK(eb.process(big.data(), big.size()).empty());
    Bytes trunc(big.begin(), big.begin() + 30);               // size field disagrees
    CHECK(eb.process(trunc.data(), trunc.size()).empty());
    Bytes tiny = ringItem(PHYSICS_EVENT, 0, 7, 0, Bytes{1, 2});
    CHECK(eb.process(tiny.data(), tiny.size()).empty());
    CHECK(eb.stats().badDropped == 3);
    CHECK(eb.takeMessages().size() == 3);
    Bytes ok = eventItem(Ev());
    CHECK(eb.process(ok.data(), ok.size()).empty() && eb.open() == 1);
  }

  if (argc == 3 && string(argv[1]) == "--write") {
    FILE* f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    for (auto& item : stream) fwrite(item.data(), 1, item.size(), f);
    fclose(f);
    cout << "wrote " << stream.size() << " items to " << argv[2] << endl;
  }

  if (failures) { cerr << failures << " check(s) failed" << endl; return 1; }
  // --- the timestamp policy: first, last, average of the fragments'
  {
    struct { EventBuilder::Timestamp policy; double us; uint16_t glom; } cases[] = {
      {EventBuilder::Timestamp::First,   10.0, GLOM_TIMESTAMP_FIRST},
      {EventBuilder::Timestamp::Last,    10.4, GLOM_TIMESTAMP_LAST},
      {EventBuilder::Timestamp::Average, 10.2, GLOM_TIMESTAMP_AVERAGE},
    };
    for (auto& c : cases) {
      EventBuilder::Config cfg = config(0x3);
      cfg.timestamp = c.policy;
      EventBuilder eb(cfg);
      Bytes begin = stateItem(BEGIN_RUN, true);
      auto outs = eb.process(begin.data(), begin.size());
      CHECK(outs.size() == 3);
      if (outs.size() == 3) {
        GlomParameters g; memcpy(&g, outs[2].data(), sizeof g);
        CHECK(g.s_timestampPolicy == c.glom);
      }
      Ev e; e.board = 1; e.tstamp_us = 10.4; Bytes late = eventItem(e);
      e.board = 0; e.tstamp_us = 10.0;       Bytes early = eventItem(e);
      eb.process(late.data(), late.size());
      outs = eb.process(early.data(), early.size());
      CHECK(outs.size() == 1);
      Ev want; want.tstamp_us = c.us;
      if (outs.size() == 1) CHECK(get<uint64_t>(outs[0], TS) == stamp(want));
    }
  }

  // --- the RING_FORMAT item each run starts with
  {
    Bytes f = EventBuilder::formatItem();
    CHECK(f.size() == sizeof(DataFormat));
    if (f.size() == sizeof(DataFormat)) {
      DataFormat d; memcpy(&d, f.data(), sizeof d);
      CHECK(d.s_header.s_size == sizeof d && d.s_header.s_type == RING_FORMAT);
      CHECK(d.s_empty == sizeof(uint32_t));
      CHECK(d.s_majorVersion == FORMAT_MAJOR && d.s_minorVersion == FORMAT_MINOR);
    }
  }

  cout << "test_builder: all checks passed" << endl;
  return 0;
}
