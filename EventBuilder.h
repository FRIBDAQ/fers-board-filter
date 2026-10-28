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

#ifndef EVENTBUILDER_H
#define EVENTBUILDER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// Builds one event per trigger out of the ring items Janus (janus-frib,
// RingBufferHandler.cpp) writes: one PHYSICS_EVENT per board and trigger,
// all with the same body-header source id.  The board items sharing a key
// -- the trigger_id, or the Janus timestamp within a window -- become the
// fragments of one built PHYSICS_EVENT in the layout glom writes, so that
// consumers of built events (SpecTcl-Janus unpackBuiltEvent, unglom) read
// it unchanged:
//
//   ring item header | body header (timestamp per Config::timestamp, sourceId,
//   barrier 0) | uint32 body bytes (itself included) | fragments
//   fragment = uint64 timestamp | uint32 sourceId (board) | uint32 bytes
//              | uint32 barrier (0) | the board's ring item, byte-identical
//              but for its body-header source id, set to the board
//
// The items of a trigger do not arrive together: Janus reads the boards in
// turn and writes a bunch from one board, then a bunch from the next, so
// several triggers are open at once.  A trigger is released, in key order,
// once every active board has contributed to it or has gone past it (sent
// a later key); at a state change; and, for a board that stopped sending,
// once more than maxPending triggers are open (oldest first).  Its
// fragments go out in timestamp order, as the event builder's orderer
// would have delivered them to glom.
//
// BEGIN/END_RUN and any other non-event item pass through after the open
// triggers are released, with the body-header source id set to sourceId
// like everything else the builder writes.  Every BEGIN_RUN is followed by
// an EVB_GLOM_INFO item describing the building (dt, timestamp policy), as
// glom writes one; formatItem() gives the RING_FORMAT item
// a stream starts with.  The Janus file-header record, written once per
// run, becomes a built event of its own with one fragment that keeps
// Janus's source id.
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
// The file-header record has no metadata word:
//   uint16 inclusiveSize | FileHeader_t | uint32 0xffffffff | pad
class EventBuilder
{
  public:
    static const unsigned kMaxBoards = 16;  // FERSlib limit (SpecTcl-Janus handles 8)

    // Trigger: the items of a trigger share its trigger_id.  Time: they
    // share the Janus timestamp (tstamp_us * 10000) within dt.  Timing-mode
    // events carry no trigger_id and always use Time.
    enum class Key { Trigger, Time };

    // Body-header timestamp of a built event: the earliest, the latest or
    // the average of its fragments' (glom's --timestamp-policy)
    enum class Timestamp { First, Last, Average };

    struct Config {
      uint32_t activeBoards = 0x1;  // bit b set -> board b is built (inverse of
                                    // the SpecTcl boardMask)
      Key      key          = Key::Time;
      uint64_t dt           = 100;    // Time window, body-header timestamp units
      size_t   maxPending   = 1000;   // open triggers before the oldest is forced
      uint32_t sourceId     = 10;     // body-header source id of all output items
      Timestamp timestamp   = Timestamp::Last;
    };

    // Counts since the last BEGIN_RUN (the whole input when there was none)
    struct Stats {
      uint64_t perBoard[kMaxBoards] = {};  // fragments built per board
      uint64_t built = 0;                  // built events (header records excluded)
      uint64_t incomplete = 0;             // built events missing an active board
      uint64_t forced = 0;                 // released by maxPending or a state change
      uint64_t duplicates = 0;             // a board twice in one built event
      uint64_t headerRecords = 0;          // Janus file-header records
      uint64_t passed = 0;                 // non-event items passed through
      uint64_t maskedDropped = 0;          // events of masked-out boards
      uint64_t badDropped = 0;             // malformed or board_id >= kMaxBoards
    };

    typedef std::vector<std::vector<uint8_t>> Items;

    explicit EventBuilder(const Config& config);

    // Items to write for this input item, in order: released built events,
    // then the item itself when it is not a board event.  Empty when the
    // item was dropped or is still being collected.
    Items process(const void* pItem, size_t nBytes);

    // Releases every open trigger (end of input).
    Items flush();

    // The RING_FORMAT item (FORMAT_MAJOR.FORMAT_MINOR of DataFormat.h) to
    // write before anything else, as glom does when it starts.
    static std::vector<uint8_t> formatItem();

    const Stats& stats() const { return m_stats; }
    const Config& config() const { return m_config; }
    size_t open() const { return m_open.size(); }
    // major*10 + minor of the last file-header record seen (33 = Janus
    // 4.2.0, 34 = Janus 5.0.1); 0 until one has been seen
    int dataFormat() const { return m_dataFormat; }

    // Warnings not reported before; the caller prints them.
    std::vector<std::string> takeMessages();

  private:
    struct View;
    struct Fragment {
      uint64_t             timestamp;
      uint32_t             sourceId;
      std::vector<uint8_t> bytes;                 // flat fragment: header and item
    };
    struct Group {
      uint64_t              key = 0;
      uint32_t              boards = 0;           // bit per contributing board
      std::vector<Fragment> frags;
    };

    void addFragment(Group& g, const uint8_t* p, size_t n, const View& v,
                     uint32_t sourceId, uint64_t timestamp);
    std::vector<uint8_t> finish(Group& g);
    std::vector<uint8_t> glomItem() const;
    void release(Items& out, bool all);
    void releaseAll(Items& out);
    bool complete(const Group& g) const;
    void once(const std::string& msg);

    Config                 m_config;
    bool                   m_useTime;      // key of the open triggers
    uint64_t               m_dt;           // 0 for Trigger
    int                    m_dataFormat;
    std::map<uint64_t, Group> m_open;      // by key
    uint64_t               m_last[kMaxBoards];  // latest key per board
    uint32_t               m_seen;         // bit per board that sent anything
    Stats                  m_stats;
    std::set<std::string>  m_said;
    std::vector<std::string> m_messages;
};

#endif
