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

#include "BoardRouter.h"

#include <DataFormat.h>
#include <cstring>

namespace {

const uint64_t kNullTimestamp = UINT64_MAX;   // NSCLDAQ NULL_TIMESTAMP

// FERS_constant.h (SpecTcl-Janus)
const int kAcqModeTiming     = 0x02;
const int kAcqModeSpecTiming = 0x03;

uint16_t get16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, sizeof v); return v; }
uint64_t get64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, sizeof v); return v; }

// Barrier type ringFragmentSource assigns to items without a body header
uint32_t barrierFor(uint32_t type)
{
  if (type == BEGIN_RUN) return 1;
  if (type == END_RUN)   return 2;
  return 0;
}

}

// Where the parts of a raw ring item are.  NSCLDAQ 12 marks "no body
// header" with a uint32 holding sizeof(uint32_t) (11.x used 0).  A body
// header may carry an extension, so the body starts s_size bytes after the
// ring item header.
struct BoardRouter::View {
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
};

BoardRouter::BoardRouter(uint32_t activeBoards, Timestamp mode)
  : m_active(activeBoards & ((1u << kMaxBoards) - 1)),
    m_mode(mode),
    m_dataFormat(0)
{
}

std::vector<BoardRouter::Output>
BoardRouter::route(const void* pItem, size_t nBytes)
{
  const uint8_t* p = static_cast<const uint8_t*>(pItem);
  std::vector<Output> out;

  View v;
  if (!v.locate(p, nBytes)) {
    m_stats.badDropped++;
    once("malformed ring item dropped (" + std::to_string(nBytes) + " bytes)");
    return out;
  }

  // State changes and anything else Janus might add: every board, with the
  // timestamp NSCLDAQ readouts give barrier items
  if (v.type != PHYSICS_EVENT) return broadcast(p, nBytes, v, &kNullTimestamp);

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
    // One copy per active board, each with its board's source id and the
    // Janus timestamp (0) kept, so every source has a fragment at 0 and glom
    // builds the copies together (with trigger 0 in trigger mode).  A single
    // copy in one ring left the orderer waiting for the other sources until
    // its timeout.  An unpacker walking the fragments tells the record apart
    // by the same byte.
    m_dataFormat = (word2 & 0xff)*10 + (word2 >> 8);
    m_stats.headerRecords++;
    for (unsigned b = 0; b < kMaxBoards; b++) {
      if (m_active & (1u << b)) out.push_back({static_cast<int>(b), rewrite(p, nBytes, v, b, nullptr)});
    }
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
  if (!(m_active & (1u << board))) {
    m_stats.maskedDropped++;
    once("ignoring board " + std::to_string(board) + " (masked out)");
    return out;
  }

  const uint64_t* pTimestamp = nullptr;
  uint64_t trigger = 0;
  if (m_mode == Timestamp::Trigger) {
    int acqmode = (word2 >> 8) & 0x0f;
    if (acqmode == kAcqModeTiming) {
      once("timing-mode events carry no trigger_id; keeping the Janus timestamps");
    } else if (acqmode == kAcqModeSpecTiming && m_dataFormat == 0) {
      // Whether DeltaTref precedes trigger_id depends on the data format,
      // which only the run's header record tells
      once("no Janus header record seen yet (started mid-run?): keeping the Janus "
           "timestamps of spectiming events until the next run starts");
    } else {
      // inclusiveSize(2) metadata(2) eventSize(2) board_id(1) tstamp_us(8)
      size_t offset = 15;
      if (word2 & 0x8000) offset += 8;        // rel_tstamp_us (Enable_2nd_tstamp)
      if (m_dataFormat >= 34 && acqmode == kAcqModeSpecTiming) offset += 8;  // DeltaTref
      if (offset + sizeof trigger > v.bodySize) {
        m_stats.badDropped++;
        once("event too short for a trigger_id dropped");
        return out;
      }
      trigger = get64(v.body + offset);
      pTimestamp = &trigger;
    }
  }

  m_stats.perBoard[board]++;
  out.push_back({static_cast<int>(board), rewrite(p, nBytes, v, board, pTimestamp)});
  return out;
}

std::vector<BoardRouter::Output>
BoardRouter::broadcast(const uint8_t* p, size_t n, const View& v,
                       const uint64_t* pTimestamp)
{
  std::vector<Output> out;
  for (unsigned b = 0; b < kMaxBoards; b++) {
    if (m_active & (1u << b)) out.push_back({static_cast<int>(b), rewrite(p, n, v, b, pTimestamp)});
  }
  m_stats.broadcast++;
  return out;
}

std::vector<uint8_t>
BoardRouter::rewrite(const uint8_t* p, size_t n, const View& v,
                     uint32_t sourceId, const uint64_t* pTimestamp)
{
  std::vector<uint8_t> out;
  if (v.bodyHeader) {
    out.assign(p, p + n);
    BodyHeader bh;
    std::memcpy(&bh, v.bodyHeader, sizeof bh);
    bh.s_sourceId = sourceId;
    if (pTimestamp) bh.s_timestamp = *pTimestamp;
    std::memcpy(out.data() + (v.bodyHeader - p), &bh, sizeof bh);   // keeps any extension
  } else {
    // Give the copy a body header so that ringFragmentSource sees this
    // board's source id instead of its --default-id
    RingItemHeader h;
    std::memcpy(&h, p, sizeof h);
    BodyHeader bh;
    bh.s_size      = sizeof bh;
    bh.s_timestamp = pTimestamp ? *pTimestamp : kNullTimestamp;
    bh.s_sourceId  = sourceId;
    bh.s_barrier   = barrierFor(h.s_type);
    h.s_size = sizeof h + sizeof bh + v.bodySize;

    out.reserve(h.s_size);
    const uint8_t* ph = reinterpret_cast<const uint8_t*>(&h);
    const uint8_t* pb = reinterpret_cast<const uint8_t*>(&bh);
    out.insert(out.end(), ph, ph + sizeof h);
    out.insert(out.end(), pb, pb + sizeof bh);
    out.insert(out.end(), v.body, v.body + v.bodySize);
  }
  return out;
}

void
BoardRouter::once(const std::string& msg)
{
  if (m_said.insert(msg).second) m_messages.push_back(msg);
}

std::vector<std::string>
BoardRouter::takeMessages()
{
  std::vector<std::string> msgs;
  msgs.swap(m_messages);
  return msgs;
}
