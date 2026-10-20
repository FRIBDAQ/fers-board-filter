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

#ifndef BOARDROUTER_H
#define BOARDROUTER_H

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// Splits the ring items written by Janus (janus-frib, RingBufferHandler.cpp)
// into one stream per FERS board.  Janus writes one PHYSICS_EVENT item per
// board and trigger, all with the same body-header source id; here each
// item is copied to its board's output with the source id set to the board
// number so that the NSCLDAQ event builder can treat every board as its
// own data source.  Everything that is not a board event (BEGIN/END_RUN,
// the Janus file-header record, ...) is copied to every board's output.
//
// Works on raw ring item bytes (DataFormat.h) so it can be tested without
// the NSCLDAQ libraries.
//
// Janus PHYSICS_EVENT body:
//   uint16 inclusiveSize | uint16 metadata | event record | uint32 0xffffffff
//   | 0xff pad to a multiple of 4 bytes
//   metadata = acqmode << 8 | timeunit   (bit 15: Enable_2nd_tstamp)
//   event record = uint16 eventSize | uint8 board_id | double tstamp_us
//                  | [double rel_tstamp_us] | [double DeltaTref]
//                  | uint64 trigger_id | ...        (no trigger_id in timing mode)
// The file-header record written once per run has no metadata word:
//   uint16 inclusiveSize | FileHeader_t | uint32 0xffffffff | pad
class BoardRouter
{
  public:
    static const unsigned kMaxBoards = 8;   // FERS_MAX_BOARDS (SpecTcl-Janus)

    // Keep: the body-header timestamp Janus assigned (tstamp_us * 10000).
    // Trigger: replace it with the event's trigger_id so that the event
    // builder matches boards by trigger number (coincidence window 0).
    enum class Timestamp { Keep, Trigger };

    struct Output {
      int                  board;
      std::vector<uint8_t> item;
    };

    struct Stats {
      uint64_t perBoard[kMaxBoards] = {};  // board events forwarded
      uint64_t broadcast = 0;              // items copied to every board
      uint64_t maskedDropped = 0;          // events of masked-out boards
      uint64_t badDropped = 0;             // malformed or board_id >= kMaxBoards
    };

    // activeBoards: bit b set -> board b is kept (inverse of the SpecTcl
    // boardMask, where a set bit masks a board out).
    BoardRouter(uint32_t activeBoards, Timestamp mode);

    // Copies of the item to emit, one per destination board.  Empty when
    // the item is dropped.
    std::vector<Output> route(const void* pItem, size_t nBytes);

    const Stats& stats() const { return m_stats; }
    uint32_t activeBoards() const { return m_active; }
    // major*10 + minor of the last file-header record seen (34 = Janus 5.0.1)
    int dataFormat() const { return m_dataFormat; }

    // Warnings not reported before; the caller prints them.
    std::vector<std::string> takeMessages();

  private:
    struct View;
    std::vector<Output> broadcast(const uint8_t* p, size_t n, const View& v);
    std::vector<uint8_t> rewrite(const uint8_t* p, size_t n, const View& v,
                                 uint32_t sourceId, const uint64_t* pTimestamp);
    void once(const std::string& msg);

    uint32_t              m_active;
    Timestamp             m_mode;
    int                   m_dataFormat;
    Stats                 m_stats;
    std::set<std::string> m_said;
    std::vector<std::string> m_messages;
};

#endif
