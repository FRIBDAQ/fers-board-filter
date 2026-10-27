/******************************************************************************
    This software is Copyright by the Board of Trustees of Michigan
    State University (c) Copyright 2026.

    You may use this software under the terms of the GNU public license
    (GPL).  The terms of this license are described at:

     http://www.gnu.org/licenses/gpl.txt

     Authors:
          Genie Jhang <changj@frib.msu.edu>

          Facility for Rare Isotope Beams
          Michigan State University
          East Lansing, MI 48824-1321
******************************************************************************/

#include "EventBuilder.h"

#include <DataFormat.h>
#include <algorithm>
#include <cstring>

namespace {

const uint64_t kNullTimestamp = UINT64_MAX;   // NSCLDAQ NULL_TIMESTAMP

// FERS_constant.h (SpecTcl-Janus)
const int kAcqModeTiming     = 0x02;
const int kAcqModeSpecTiming = 0x03;

// NSCLDAQ event builder flat fragment header (fragment.h, EVB::FragmentHeader)
struct __attribute__((packed)) FragmentHeader {   // 20 bytes, no padding
  uint64_t s_timestamp;
  uint32_t s_sourceId;
  uint32_t s_size;
  uint32_t s_barrier;
};

uint16_t get16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, sizeof v); return v; }
uint64_t get64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, sizeof v); return v; }

template <typename T> void append(std::vector<uint8_t>& out, const T& v)
{
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof v);
}

}

// Where the parts of a raw ring item are.  NSCLDAQ 12 marks "no body
// header" with a uint32 holding sizeof(uint32_t) (11.x used 0).  A body
// header may carry an extension, so the body starts s_size bytes after the
// ring item header.
struct EventBuilder::View {
  uint32_t       type;
  const uint8_t* bodyHeader;   // nullptr when the item has none
  const uint8_t* body;
  size_t         bodySize;

  bool locate(const uint8_t* p, size_t n)
  {
    if (n < sizeof(RingItemHeader) + sizeof(uint32_t)) return false;
    RingItemHeader h;
    std::memcpy(&h, p, sizeof h);
    if (h.s_size != n) return false;
    type = h.s_type;

    uint32_t bhSize;
    std::memcpy(&bhSize, p + sizeof h, sizeof bhSize);
    if (bhSize > sizeof(uint32_t)) {
      if (bhSize < sizeof(BodyHeader) || sizeof h + bhSize > n) return false;
      bodyHeader = p + sizeof h;
      body = bodyHeader + bhSize;
    } else {
      bodyHeader = nullptr;
      body = p + sizeof h + sizeof(uint32_t);
    }
    bodySize = n - (body - p);
    return true;
  }

  uint64_t timestamp() const
  {
    if (!bodyHeader) return kNullTimestamp;
    BodyHeader bh;
    std::memcpy(&bh, bodyHeader, sizeof bh);
    return bh.s_timestamp;
  }
};

EventBuilder::EventBuilder(const Config& config)
  : m_config(config),
    m_useTime(config.key == Key::Time),
    m_dt(m_useTime ? config.dt : 0),
    m_dataFormat(0),
    m_seen(0)
{
  m_config.activeBoards &= (1u << kMaxBoards) - 1;
  for (unsigned b = 0; b < kMaxBoards; b++) m_last[b] = 0;
}

EventBuilder::Items
EventBuilder::process(const void* pItem, size_t nBytes)
{
  const uint8_t* p = static_cast<const uint8_t*>(pItem);
  Items out;

  View v;
  if (!v.locate(p, nBytes)) {
    m_stats.badDropped++;
    once("malformed ring item dropped");
    return out;
  }

  // State changes and anything else Janus might add close the open
  // triggers (a run boundary) and pass through unchanged
  if (v.type != PHYSICS_EVENT) {
    releaseAll(out);
    m_seen = 0;
    m_stats.passed++;
    out.emplace_back(p, p + nBytes);
    return out;
  }

  // After inclusiveSize an event carries the metadata word, the file-header
  // record the dataformat major and minor bytes.  The low byte of the
  // metadata is the time unit (0 or 1); the record's is the major version.
  if (v.bodySize < 4) {
    m_stats.badDropped++;
    once("PHYSICS_EVENT shorter than a Janus record dropped");
    return out;
  }
  uint16_t word2 = get16(v.body + 2);
  if ((word2 & 0xff) > 1) {
    // A built event of its own: the record is not a board's, so the
    // fragment keeps the source id Janus gave it.  A new run starts here;
    // its events may use another key than the previous run's.
    releaseAll(out);
    m_seen = 0;
    m_dataFormat = (word2 & 0xff)*10 + (word2 >> 8);
    m_stats.headerRecords++;
    Group g;
    BodyHeader bh;
    uint32_t sid = m_config.sourceId;
    if (v.bodyHeader) { std::memcpy(&bh, v.bodyHeader, sizeof bh); sid = bh.s_sourceId; }
    addFragment(g, p, nBytes, v, sid, v.timestamp());
    out.push_back(finish(g));
    return out;
  }

  if (v.bodySize < 7) {
    m_stats.badDropped++;
    once("PHYSICS_EVENT shorter than a Janus event header dropped");
    return out;
  }
  unsigned board = v.body[6];
  if (board >= kMaxBoards) {
    m_stats.badDropped++;
    once("board_id " + std::to_string(board) + " exceeds "
         + std::to_string(kMaxBoards - 1) + "; events dropped");
    return out;
  }
  if (!(m_config.activeBoards & (1u << board))) {
    m_stats.maskedDropped++;
    once("ignoring board " + std::to_string(board) + " (masked out)");
    return out;
  }
  if (!v.bodyHeader) {
    m_stats.badDropped++;
    once("event without a body header dropped");
    return out;
  }
  uint64_t timestamp = v.timestamp();

  // The key of this event: its trigger_id, or its timestamp when the
  // configuration, the acquisition mode or a missing header record says so
  bool useTime = m_config.key == Key::Time;
  int acqmode = (word2 >> 8) & 0x0f;
  uint64_t key = timestamp;
  if (!useTime) {
    if (acqmode == kAcqModeTiming) {
      once("timing-mode events carry no trigger_id; building by timestamp");
      useTime = true;
    } else if (acqmode == kAcqModeSpecTiming && m_dataFormat == 0) {
      // Whether DeltaTref precedes trigger_id depends on the data format,
      // which only the run's header record tells
      once("no Janus header record seen yet (started mid-run?): building "
           "spectiming events by timestamp until the next run starts");
      useTime = true;
    } else {
      // inclusiveSize(2) metadata(2) eventSize(2) board_id(1) tstamp_us(8)
      size_t offset = 15;
      if (word2 & 0x8000) offset += 8;        // rel_tstamp_us (Enable_2nd_tstamp)
      if (m_dataFormat >= 34 && acqmode == kAcqModeSpecTiming) offset += 8;  // DeltaTref
      if (offset + sizeof key > v.bodySize) {
        m_stats.badDropped++;
        once("event too short for a trigger_id dropped");
        return out;
      }
      key = get64(v.body + offset);
    }
  }
  if (useTime != m_useTime) {
    // Keys of the two kinds cannot share the open set
    releaseAll(out);
    m_seen = 0;
    m_useTime = useTime;
    m_dt = useTime ? m_config.dt : 0;
  }

  // The open trigger this event belongs to: the same key, or (Time) a key
  // within dt of its timestamp; else a new one
  std::map<uint64_t, Group>::iterator it;
  if (m_dt == 0) {
    it = m_open.find(key);
  } else {
    it = m_open.lower_bound(key > m_dt ? key - m_dt : 0);
    if (it != m_open.end() && it->first > key + m_dt) it = m_open.end();
  }
  if (it == m_open.end()) {
    it = m_open.emplace(key, Group()).first;
    it->second.key = key;
  }
  Group& g = it->second;
  if (g.boards & (1u << board)) m_stats.duplicates++;
  g.boards |= 1u << board;
  addFragment(g, p, nBytes, v, board, timestamp);
  m_stats.perBoard[board]++;

  if (!(m_seen & (1u << board)) || key > m_last[board]) m_last[board] = key;
  m_seen |= 1u << board;

  release(out, false);
  return out;
}

EventBuilder::Items
EventBuilder::flush()
{
  Items out;
  releaseAll(out);
  m_seen = 0;
  return out;
}

// Every active board has contributed or has sent a later key
bool
EventBuilder::complete(const Group& g) const
{
  for (unsigned b = 0; b < kMaxBoards; b++) {
    uint32_t bit = 1u << b;
    if (!(m_config.activeBoards & bit) || (g.boards & bit)) continue;
    if (!(m_seen & bit) || m_last[b] <= g.key + m_dt) return false;
  }
  return true;
}

// Releases, oldest first, the triggers that are complete (all: every one),
// and the oldest while more than maxPending are open
void
EventBuilder::release(Items& out, bool all)
{
  while (!m_open.empty()) {
    Group& g = m_open.begin()->second;
    bool forced = all || m_open.size() > m_config.maxPending;
    if (!forced && !complete(g)) break;
    if (forced && (g.boards & m_config.activeBoards) != m_config.activeBoards) m_stats.forced++;
    if ((g.boards & m_config.activeBoards) != m_config.activeBoards) m_stats.incomplete++;
    m_stats.built++;
    out.push_back(finish(g));
    m_open.erase(m_open.begin());
  }
}

void
EventBuilder::releaseAll(Items& out)
{
  release(out, true);
}

// Appends the item as a flat fragment, with its body-header source id set
// to sourceId (an item without one gets a body header, so that the
// fragment's payload has the source id too)
void
EventBuilder::addFragment(Group& g, const uint8_t* p, size_t n, const View& v,
                          uint32_t sourceId, uint64_t timestamp)
{
  g.frags.emplace_back();
  Fragment& f = g.frags.back();
  f.timestamp = timestamp;
  f.sourceId  = sourceId;
  std::vector<uint8_t>& out = f.bytes;
  FragmentHeader fh;
  fh.s_timestamp = timestamp;
  fh.s_sourceId  = sourceId;
  fh.s_barrier   = 0;
  fh.s_size      = 0;                              // patched below
  append(out, fh);

  if (v.bodyHeader) {
    out.insert(out.end(), p, p + n);
    BodyHeader bh;
    std::memcpy(&bh, v.bodyHeader, sizeof bh);
    bh.s_sourceId = sourceId;
    std::memcpy(out.data() + sizeof fh + (v.bodyHeader - p), &bh, sizeof bh);   // keeps any extension
  } else {
    RingItemHeader h;
    std::memcpy(&h, p, sizeof h);
    BodyHeader bh;
    bh.s_size      = sizeof bh;
    bh.s_timestamp = timestamp;
    bh.s_sourceId  = sourceId;
    bh.s_barrier   = 0;
    h.s_size = sizeof h + sizeof bh + v.bodySize;
    append(out, h);
    append(out, bh);
    out.insert(out.end(), v.body, v.body + v.bodySize);
  }
  fh.s_size = out.size() - sizeof fh;
  std::memcpy(out.data(), &fh, sizeof fh);

  if (timestamp != kNullTimestamp && timestamp < g.firstTs) g.firstTs = timestamp;
}

// The built event, as glom writes it: fragments in timestamp order (the
// orderer's), equal timestamps by source id
std::vector<uint8_t>
EventBuilder::finish(Group& g)
{
  std::stable_sort(g.frags.begin(), g.frags.end(),
                   [](const Fragment& a, const Fragment& b) {
                     return a.timestamp != b.timestamp ? a.timestamp < b.timestamp
                                                       : a.sourceId < b.sourceId;
                   });
  size_t fragBytes = 0;
  for (auto& f : g.frags) fragBytes += f.bytes.size();

  RingItemHeader h;
  BodyHeader bh;
  uint32_t bodyBytes = sizeof(uint32_t) + fragBytes;
  h.s_type = PHYSICS_EVENT;
  h.s_size = sizeof h + sizeof bh + bodyBytes;
  bh.s_size      = sizeof bh;
  bh.s_timestamp = g.firstTs == UINT64_MAX ? kNullTimestamp : g.firstTs;
  bh.s_sourceId  = m_config.sourceId;
  bh.s_barrier   = 0;

  std::vector<uint8_t> out;
  out.reserve(h.s_size);
  append(out, h);
  append(out, bh);
  append(out, bodyBytes);
  for (auto& f : g.frags) out.insert(out.end(), f.bytes.begin(), f.bytes.end());
  return out;
}

void
EventBuilder::once(const std::string& msg)
{
  if (m_said.insert(msg).second) m_messages.push_back(msg);
}

std::vector<std::string>
EventBuilder::takeMessages()
{
  std::vector<std::string> msgs;
  msgs.swap(m_messages);
  return msgs;
}
