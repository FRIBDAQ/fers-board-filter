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

// fersBoardFilter INRING MASK
//
// Reads the ring Janus writes to and copies every board's PHYSICS_EVENT
// items into the ring INRING_<board> with the body-header source id set to
// the board number; BEGIN/END_RUN and the Janus file-header record go to
// every board ring.  The event builder (ReadoutCallouts.tcl) then takes
// each INRING_<board> as one data source.

#include "BoardRouter.h"

#include <CDataSource.h>
#include <CDataSourceFactory.h>
#include <CRingBuffer.h>
#include <CRingItem.h>
#include <DataFormat.h>
#include <Exception.h>

#include <getopt.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

using namespace std;

static const char* kProgram = "fersBoardFilter";

static void
usage(ostream& o)
{
  o << "Usage: " << kProgram << " [options] INRING MASK\n"
       "  INRING  ring Janus writes to: a local ring name or a URL (tcp://host/name,\n"
       "          file:///path/run.evt)\n"
       "  MASK    active boards, bit b set = board b kept (0x3: boards 0 and 1).\n"
       "          Note: the SpecTcl boardMask has the opposite sense.\n"
       "Options:\n"
       "  --timestamp=keep     keep the Janus body-header timestamp (tstamp_us*10000) [default]\n"
       "  --timestamp=trigger  use the event's trigger_id as timestamp (glom dt 0 then\n"
       "                       builds by trigger number; not possible in timing mode)\n"
       "  --help\n"
       "Each active board b gets the output ring <name>_<b> on this host, where <name>\n"
       "is the ring part of INRING; it is created when it does not exist.\n";
}

static void
report(const BoardRouter& router, ostream& o)
{
  const BoardRouter::Stats& s = router.stats();
  o << kProgram << ": events";
  for (unsigned b = 0; b < BoardRouter::kMaxBoards; b++) {
    if (router.activeBoards() & (1u << b)) o << " board" << b << "=" << s.perBoard[b];
  }
  o << "; to all boards " << s.broadcast << "; dropped masked " << s.maskedDropped
    << ", bad " << s.badDropped << "; dataformat " << router.dataFormat() << endl;
}

int
main(int argc, char** argv)
{
  BoardRouter::Timestamp mode = BoardRouter::Timestamp::Keep;
  static const option longopts[] = {
    {"timestamp", required_argument, nullptr, 't'},
    {"help",      no_argument,       nullptr, 'h'},
    {nullptr, 0, nullptr, 0}
  };
  int c;
  while ((c = getopt_long(argc, argv, "t:h", longopts, nullptr)) != -1) {
    switch (c) {
      case 't':
        if (!strcmp(optarg, "keep"))         mode = BoardRouter::Timestamp::Keep;
        else if (!strcmp(optarg, "trigger")) mode = BoardRouter::Timestamp::Trigger;
        else { cerr << kProgram << ": --timestamp must be keep or trigger\n"; return 2; }
        break;
      case 'h': usage(cout); return 0;
      default:  usage(cerr); return 2;
    }
  }
  if (argc - optind != 2) { usage(cerr); return 2; }

  string source = argv[optind];
  char* end = nullptr;
  unsigned long mask = strtoul(argv[optind + 1], &end, 0);
  if (*argv[optind + 1] == '\0' || *end != '\0') {
    cerr << kProgram << ": MASK must be a number (e.g. 0x3)\n"; return 2;
  }
  uint32_t active = mask & ((1u << BoardRouter::kMaxBoards) - 1);
  if (active == 0) { cerr << kProgram << ": MASK keeps no board\n"; return 2; }
  if (mask != active) {
    cerr << kProgram << ": only boards 0-" << BoardRouter::kMaxBoards - 1
         << " exist; ignoring the upper bits of the mask\n";
  }

  // A bare name is a ring on this host; the output rings take its name
  string uri = source.find("://") == string::npos ? "tcp://localhost/" + source : source;
  string name = uri.substr(uri.rfind('/') + 1);
  if (name.empty()) { cerr << kProgram << ": cannot derive a ring name from " << uri << "\n"; return 2; }

  try {
    map<int, unique_ptr<CRingBuffer>> rings;
    for (unsigned b = 0; b < BoardRouter::kMaxBoards; b++) {
      if (!(active & (1u << b))) continue;
      string ringName = name + "_" + to_string(b);
      if (CRingBuffer::isRing(ringName)) {
        rings[b].reset(new CRingBuffer(ringName, CRingBuffer::producer));
      } else {
        rings[b].reset(CRingBuffer::createAndProduce(ringName));
        cerr << kProgram << ": created ring " << ringName << endl;
      }
    }

    unique_ptr<CDataSource> src(CDataSourceFactory::makeSource(uri, {}, {}));
    BoardRouter router(active, mode);
    cerr << kProgram << ": reading " << uri << ", boards";
    for (unsigned b = 0; b < BoardRouter::kMaxBoards; b++) {
      if (active & (1u << b)) cerr << " " << b << " -> " << name << "_" << b;
    }
    cerr << endl;

    while (true) {
      unique_ptr<CRingItem> item(src->getItem());
      if (!item) break;                     // end of a file source

      for (auto& out : router.route(item->getItemPointer(), item->size())) {
        rings[out.board]->put(out.item.data(), out.item.size());
      }
      for (auto& msg : router.takeMessages()) cerr << kProgram << ": " << msg << endl;
      if (item->type() == END_RUN) report(router, cerr);
    }
    report(router, cerr);
  }
  catch (CException& e) { cerr << kProgram << ": " << e.ReasonText() << endl; return 1; }
  catch (exception& e)  { cerr << kProgram << ": " << e.what() << endl; return 1; }
  catch (string& e)     { cerr << kProgram << ": " << e << endl; return 1; }
  catch (const char* e) { cerr << kProgram << ": " << e << endl; return 1; }
  return 0;
}
