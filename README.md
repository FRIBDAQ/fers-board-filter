# fersBoardFilter

Splits the ring Janus (janus-frib) writes into one ring per FERS board so that
the NSCLDAQ event builder can assemble the boards' events per trigger.

Janus writes one `PHYSICS_EVENT` ring item per board and trigger, all with the
same body-header source id, and the boards' items arrive in bunches (several
from one board, then several from the other) rather than board by board.
`fersBoardFilter` reads that ring and

* copies each board event into the ring `<inring>_<board>` with the body-header
  source id set to the board number (the item is otherwise byte-identical);
* copies BEGIN/END_RUN (and any other non-event item) to every board ring,
  source id per ring and timestamp `NULL_TIMESTAMP`, as NSCLDAQ readouts stamp
  barrier items (Janus puts wall-clock seconds there);
* copies the Janus file-header record, written once per run, to every board
  ring with the source id per ring and its timestamp 0 kept, so every
  event-builder source has a fragment at 0 (a single copy in one ring left
  the orderer waiting for the other sources until its timeout);
* creates the output rings – and a local input ring – when they do not exist,
  so it can be started before Janus;
* drops events of boards that are not in the mask (one notice per board) and
  malformed items (counted, reported at END_RUN).

The event builder set up in `ReadoutCallouts.tcl` then takes each
`<inring>_<board>` as one data source and writes built events to
`<inring>_event`.  SpecTcl no longer needs the `boardMask` merging: every built
event already holds one fragment per active board (plus, once per run, one
built event holding a copy of the header record per board – in trigger mode
it is the built event of trigger 0).

## Files

| file | what |
|---|---|
| `fersBoardFilter.cpp`, `BoardRouter.{h,cpp}` | the filter program |
| `test_router.cpp` | unit tests of the routing (`make test`) |
| `FERSBoardFilter.tcl`, `pkgIndex.tcl` | ReadoutShell package that runs the filter |
| `FERSBoardFilterSettings.tcl` | ring name, board mask, timestamp mode |
| `ReadoutCallouts.tcl` | runs the filter and the event builder from ReadoutShell |

## Build

Needs an NSCLDAQ 12 installation (headers, `libdaqio`, `libdataformat`,
`libDataFlow`, `libdaqshm`, `liburl`, `libException`).

    source /usr/opt/daq/12.x/daqsetup.bash     # sets DAQROOT
    make            # fersBoardFilter
    make test       # router checks; needs only DataFormat.h

## Running from ReadoutShell (recommended)

1. Edit `FERSBoardFilterSettings.tcl`: `inring` (the ring Janus writes to,
   "Output RingBuffer" of the Janus provider), `boardMask` (bit *b* set =
   board *b*), `timestamp` (`keep` or `trigger`, see below).
2. Edit `glomdt` in `ReadoutCallouts.tcl` (see below) and point ReadoutShell at
   that `ReadoutCallouts.tcl` (or merge its contents into yours).

`FERSBoardFilter.tcl` is a run-state callout bundle built on the NSCLDAQ
`Actions`/`DefaultActions` packages, the same way as the MDPP software-trigger
launcher: when a run starts (Halted → Active) and the filter is not running it

* kills a producer left on an output ring by an earlier session,
* starts `fersBoardFilter --timestamp=<timestamp> <inring> <boardMask>` from
  this directory with its output piped through an `Actions` parser, so the
  filter's messages appear in the ReadoutGUI log with their severity
  (the filter writes `LOGMSG`/`WRNMSG`/`ERRMSG <length> <text>` packets),
* waits (up to 5 s) until every `<inring>_<board>` exists, so the event
  builder's ring sources – started by the next callout bundle – find them.

The filter keeps running between runs and reports per-board counts at every
END_RUN; if it exits, a message box appears and it is started again at the
next run start.  `FERSBoardFilter::register` must run before
`::EVBC::useEventBuilder` so the rings exist before the ring sources start.

`ReadoutCallouts.tcl` registers one event-builder ring source per active board
(`tcp://localhost/<inring>_<board>`, source id = board, body headers expected,
one END_RUN per run) and starts glom with `-glombuild true -glomdt $glomdt
-glomid 100 -destring <inring>_event`.

## Running by hand

    fersBoardFilter [--timestamp=keep|trigger] INRING MASK

* `INRING` – the ring Janus writes to: a local ring name or a URL
  (`tcp://host/name`, `file:///path/run.evt`).  The output rings are named
  after the ring part of it and live on the host the filter runs on.
* `MASK` – active boards, bit *b* set = board *b* kept, e.g. `0x3` for boards
  0 and 1; up to 16 boards.  (Note: the SpecTcl `boardMask` tree variable has
  the opposite sense – a set bit masks a board out.)
* `--timestamp=keep` (default) keeps the body-header timestamp Janus assigns,
  `tstamp_us * 10000` (1 µs = 10000).  `--timestamp=trigger` replaces it with
  the event's `trigger_id`, so `glom -dt 0` builds exactly by trigger number.
  Timing-mode events carry no trigger id; they keep the Janus timestamp and
  the filter says so once.  Whether a spectiming event carries `DeltaTref`
  before the trigger id depends on the data format, which only the run's
  header record tells: started mid-run, the filter keeps the Janus timestamps
  of spectiming events until the next run starts (said once).

Example, Janus writing to ring `input`, boards 0 and 1:

    fersBoardFilter input 0x3
    # -> rings input_0 (source id 0) and input_1 (source id 1)

It exits only when a file source ends.  `CRingBuffer::put` blocks while an
output ring is full (a stalled consumer), which then also stalls Janus's ring;
with no consumer attached nothing blocks.

## Data layout the filter relies on

From `RingBufferHandler.cpp` (janus-frib) and `CFERSA5202Unpacker.cpp`
(SpecTcl-Janus):

    PHYSICS_EVENT body = uint16 inclusiveSize | uint16 metadata | record
                         | uint32 0xffffffff | 0xff pad to 4 bytes
    metadata           = acqmode << 8 | timeunit, bit 15 = Enable_2nd_tstamp
    record             = uint16 eventSize | uint8 board_id | double tstamp_us
                         | [double rel_tstamp_us] | [double DeltaTref]
                         | uint64 trigger_id | ...   (no trigger_id in timing mode)

The file-header record (once per run) has no metadata word; it is told from an
event by the low byte after `inclusiveSize`: the dataformat major (3) versus a
time unit (0 or 1).  `DeltaTref` exists for spectiming events of data format
3.4.0 (Janus 5.0.1) and later; the format is read from the header record.

## Tests

`make test` builds items with the Janus layout (as `RingBufferHandler` writes
them) and checks routing, source-id rewriting, byte identity, the trigger-id
offsets for every record layout and data format 3.3/3.4, timing mode, the
32-byte timing-event/header-record ambiguity, header-record placement,
NULL_TIMESTAMP on state changes, body-header insertion for header-less items,
body-header extensions, masked and malformed input.  `./test_router --write
run.evt` writes the test stream as an event file usable as a `file:///…`
source.
