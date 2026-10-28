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

// fersBoardFilter [options] INRING MASK
//
// Reads the ring Janus writes to, builds the boards' PHYSICS_EVENT items of
// a trigger into one built event (the layout glom writes, one fragment per
// board with the source id set to the board number) and writes the built
// events to the ring INRING_event; BEGIN/END_RUN pass through unchanged.
// See EventBuilder.h for the building rules.
//
// Messages go to stderr in the ReadoutGUI Actions protocol
// ("LOGMSG <length> <text>", see NSCLDAQ Actions.cpp), so that the
// FERSBoardFilter Tcl package can show them in the ReadoutGUI log; read on
// a terminal they are still plain text.

#include "EventBuilder.h"

#include <CDataSource.h>
#include <CDataSourceFactory.h>
#include <CRingBuffer.h>
#include <CRingItem.h>
#include <DataFormat.h>
#include <Exception.h>

#include <getopt.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace std;

static const char* kProgram = "fersBoardFilter";

// One Actions packet; directive is LOGMSG, WRNMSG or ERRMSG.  Messages must
// not contain newlines.
static void
say(const char* directive, const string& msg)
{
  cerr << directive << " " << msg.size() << " " << msg << "\n" << flush;
}
static void logMsg(const string& m)  { say("LOGMSG", m); }
static void warnMsg(const string& m) { say("WRNMSG", m); }
static void errMsg(const string& m)  { say("ERRMSG", m); }

static void
usage(ostream& o)
{
  EventBuilder::Config d;
  o << "Usage: " << kProgram << " [options] INRING MASK\n"
       "  INRING  ring Janus writes to: a local ring name or a URL (tcp://host/name,\n"
       "          file:///path/run.evt)\n"
       "  MASK    active boards, bit b set = board b built (0x3: boards 0 and 1).\n"
       "          Note: the SpecTcl boardMask has the opposite sense.\n"
       "Options:\n"
       "  --key=time       the items of a trigger share the Janus timestamp within\n"
       "                   --dt [default]\n"
       "  --key=trigger    the items of a trigger share its trigger_id (only when the\n"
       "                   boards count a common trigger; not possible in timing mode)\n"
       "  --dt=N           time window, body-header timestamp units: 1 us = 10000\n"
       "                   [" << d.dt << "]\n"
       "  --max-pending=N  open triggers before the oldest is released although a\n"
       "                   board has not gone past it [" << d.maxPending << "]\n"
       "  --sourceid=N     body-header source id of all output items [" << d.sourceId << "]\n"
       "  --timestamp=P    body-header timestamp of a built event: first, last or\n"
       "                   average of its fragments' [last]\n"
       "  --outring=NAME   output ring on this host [<name>_event, <name> the ring part\n"
       "                   of INRING]\n"
       "  --help\n"
       "The output ring is created when it does not exist.  A local INRING that does\n"
       "not exist yet is created too, so the filter can be started before Janus.\n";
}

static string
report(const EventBuilder& b)
{
  const EventBuilder::Stats& s = b.stats();
  ostringstream o;
  o << "built " << s.built << " events (" << s.incomplete << " missing a board, "
    << s.forced << " forced, " << s.duplicates << " with a board twice); fragments";
  for (unsigned i = 0; i < EventBuilder::kMaxBoards; i++) {
    if (b.config().activeBoards & (1u << i)) o << " board" << i << "=" << s.perBoard[i];
  }
  o << "; header records " << s.headerRecords << "; passed through " << s.passed
    << "; dropped masked " << s.maskedDropped << ", bad " << s.badDropped
    << "; dataformat " << b.dataFormat();
  return o.str();
}

// "run N" and, when the item carries one, its title.  The body starts
// with the run number in every NSCLDAQ version; the title follows the
// 12.x s_originalSid or, in an 11.x body (4 bytes shorter), comes right
// after s_offsetDivisor.
static string
runOf(const CRingItem& item)
{
  const uint8_t* body = static_cast<const uint8_t*>(item.getBodyPointer());
  size_t size = item.getBodySize();
  if (size < sizeof(uint32_t)) return "run ?";
  uint32_t run;
  memcpy(&run, body, sizeof run);
  string result = "run " + to_string(run);
  size_t titleOffset = 0;
  if (size == sizeof(StateChangeItemBody))     titleOffset = offsetof(StateChangeItemBody, s_title);
  if (size == sizeof(StateChangeItemBody) - 4) titleOffset = offsetof(StateChangeItemBody, s_title) - 4;
  if (titleOffset) {
    string title(reinterpret_cast<const char*>(body + titleOffset),
                 strnlen(reinterpret_cast<const char*>(body + titleOffset), size - titleOffset));
    if (!title.empty()) result += " \"" + title + "\"";
  }
  return result;
}

static bool
number(const char* s, unsigned long& v)
{
  char* end = nullptr;
  v = strtoul(s, &end, 0);
  return *s != '\0' && *end == '\0';
}

int
main(int argc, char** argv)
{
  EventBuilder::Config config;
  string outring;
  static const option longopts[] = {
    {"key",         required_argument, nullptr, 'k'},
    {"dt",          required_argument, nullptr, 'd'},
    {"max-pending", required_argument, nullptr, 'p'},
    {"sourceid",    required_argument, nullptr, 's'},
    {"timestamp",   required_argument, nullptr, 't'},
    {"outring",     required_argument, nullptr, 'o'},
    {"help",        no_argument,       nullptr, 'h'},
    {nullptr, 0, nullptr, 0}
  };
  int c;
  unsigned long v;
  while ((c = getopt_long(argc, argv, "k:d:p:s:t:o:h", longopts, nullptr)) != -1) {
    switch (c) {
      case 'k':
        if (!strcmp(optarg, "time"))         config.key = EventBuilder::Key::Time;
        else if (!strcmp(optarg, "trigger")) config.key = EventBuilder::Key::Trigger;
        else { errMsg("--key must be time or trigger"); return 2; }
        break;
      case 'd':
        if (!number(optarg, v)) { errMsg("--dt must be a number"); return 2; }
        config.dt = v;
        break;
      case 'p':
        if (!number(optarg, v) || v == 0) { errMsg("--max-pending must be a number > 0"); return 2; }
        config.maxPending = v;
        break;
      case 's':
        if (!number(optarg, v)) { errMsg("--sourceid must be a number"); return 2; }
        config.sourceId = v;
        break;
      case 't':
        if (!strcmp(optarg, "first"))        config.timestamp = EventBuilder::Timestamp::First;
        else if (!strcmp(optarg, "last"))    config.timestamp = EventBuilder::Timestamp::Last;
        else if (!strcmp(optarg, "average")) config.timestamp = EventBuilder::Timestamp::Average;
        else { errMsg("--timestamp must be first, last or average"); return 2; }
        break;
      case 'o': outring = optarg; break;
      case 'h': usage(cout); return 0;
      default:  usage(cerr); return 2;
    }
  }
  if (argc - optind != 2) { usage(cerr); return 2; }

  string source = argv[optind];
  unsigned long mask;
  if (!number(argv[optind + 1], mask)) {
    errMsg("MASK must be a number (e.g. 0x3)"); return 2;
  }
  config.activeBoards = mask & ((1u << EventBuilder::kMaxBoards) - 1);
  if (config.activeBoards == 0) { errMsg("MASK keeps no board"); return 2; }
  if (mask != config.activeBoards) {
    warnMsg("only boards 0-" + to_string(EventBuilder::kMaxBoards - 1)
            + " exist; ignoring the upper bits of the mask");
  }

  // A bare name is a ring on this host; the output ring takes its name
  bool local = source.find("://") == string::npos;
  string uri = local ? "tcp://localhost/" + source : source;
  string name = uri.substr(uri.rfind('/') + 1);
  if (name.empty()) { errMsg("cannot derive a ring name from " + uri); return 2; }
  if (outring.empty()) outring = name + "_event";

  try {
    // Janus only creates its ring at the start of a run; make it so that
    // the filter can be started first (Janus then attaches as producer)
    if (local && !CRingBuffer::isRing(name)) {
      CRingBuffer::create(name);
      logMsg("created ring " + name);
    }

    unique_ptr<CRingBuffer> ring;
    if (CRingBuffer::isRing(outring)) {
      ring.reset(new CRingBuffer(outring, CRingBuffer::producer));
    } else {
      ring.reset(CRingBuffer::createAndProduce(outring));
      logMsg("created ring " + outring);
    }

    unique_ptr<CDataSource> src(CDataSourceFactory::makeSource(uri, {}, {}));
    EventBuilder builder(config);
    {
      ostringstream o;
      o << "reading " << uri << ", building boards";
      for (unsigned b = 0; b < EventBuilder::kMaxBoards; b++) {
        if (config.activeBoards & (1u << b)) o << " " << b;
      }
      o << " by " << (config.key == EventBuilder::Key::Time
                      ? "timestamp, dt " + to_string(config.dt) : string("trigger_id"))
        << " into " << outring << " (source id " << config.sourceId
        << ", timestamp " << (config.timestamp == EventBuilder::Timestamp::First ? "first"
                              : config.timestamp == EventBuilder::Timestamp::Last ? "last" : "average")
        << ", max " << config.maxPending << " open)";
      logMsg(o.str());
    }

    {
      vector<uint8_t> format = EventBuilder::formatItem();
      ring->put(format.data(), format.size());
    }
    while (true) {
      unique_ptr<CRingItem> item(src->getItem());
      if (!item) break;                     // end of a file source

      for (auto& out : builder.process(item->getItemPointer(), item->size())) {
        ring->put(out.data(), out.size());
      }
      for (auto& msg : builder.takeMessages()) warnMsg(msg);
      if (item->type() == BEGIN_RUN) logMsg("begin " + runOf(*item));
      if (item->type() == END_RUN)   logMsg("end of " + runOf(*item) + ": " + report(builder));
    }
    for (auto& out : builder.flush()) ring->put(out.data(), out.size());
    logMsg("end of input: " + report(builder));
  }
  catch (CException& e) { errMsg(e.ReasonText()); return 1; }
  catch (exception& e)  { errMsg(e.what()); return 1; }
  catch (string& e)     { errMsg(e); return 1; }
  catch (const char* e) { errMsg(e); return 1; }
  return 0;
}
