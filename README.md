# fersBoardFilter

Splits the ring Janus (janus-frib) writes into one ring per FERS board so that
the NSCLDAQ event builder can assemble the boards' events per trigger.

Janus writes one `PHYSICS_EVENT` ring item per board and trigger, all with the
same body-header source id, and the boards' items arrive in bunches (several
from one board, then several from the other) rather than board by board.
`fersBoardFilter` reads that ring and

* copies each board event into the ring `<inring>_<board>` with the body-header
  source id set to the board number (the item is otherwise byte-identical);
* copies everything else (BEGIN/END_RUN, the Janus file-header record, ...) to
  every board ring, again with the source id set per ring;
* creates the output rings when they do not exist;
* drops events of boards that are not in the mask (one notice per board) and
  malformed items (counted, reported at END_RUN).

The event builder set up in `ReadoutCallouts.tcl` then takes each
`<inring>_<board>` as one data source and writes built events to
`<inring>_event`.  SpecTcl no longer needs the `boardMask` merging: every built
event already holds one fragment per active board.

## Build

Needs an NSCLDAQ 12 installation (headers, `libdaqio`, `libdataformat`,
`libDataFlow`, `libdaqshm`, `liburl`, `libException`).

    source /usr/opt/daq/12.x/daqsetup.bash     # sets DAQROOT
    make            # fersBoardFilter
    make test       # router checks; needs only DataFormat.h

## Run

    fersBoardFilter [--timestamp=keep|trigger] INRING MASK

* `INRING` – the ring Janus writes to (`RingBufferName` in the Janus config /
  "Output RingBuffer" of the ReadoutShell Janus provider): a local ring name or
  a URL (`tcp://host/name`, `file:///path/run.evt`).  The output rings are
  named after the ring part of it and live on the host the filter runs on.
* `MASK` – active boards, bit *b* set = board *b* kept, e.g. `0x3` for boards
  0 and 1.  (Note: the SpecTcl `boardMask` tree variable has the opposite
  sense – a set bit masks a board out.)
* `--timestamp=keep` (default) keeps the body-header timestamp Janus assigns,
  `tstamp_us * 10000` (1 µs = 10000).  `--timestamp=trigger` replaces it with
  the event's `trigger_id`, so `glom -dt 0` builds exactly by trigger number.
  Timing-mode events carry no trigger id; they keep the Janus timestamp and
  the filter says so once.

Example, Janus writing to ring `input`, boards 0 and 1:

    fersBoardFilter input 0x3
    # -> rings input_0 (source id 0) and input_1 (source id 1)

Start it before the first run (the rings must exist when ReadoutShell starts
the ring sources) and leave it running; it exits only when a file source ends.
At every END_RUN it prints the per-board counts.

## ReadoutCallouts.tcl

`ReadoutCallouts.tcl` in this directory registers one event-builder ring
source per active board (`tcp://localhost/<inring>_<board>`, source id =
board, body headers expected, one END_RUN per run) and starts glom with
`-glombuild true -glomdt $glomdt -destring <inring>_event`.  Set `inring`,
`boardMask` and `glomdt` at the top to match the filter's arguments.

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
3.4.0 (Janus 5.0.1) and later; the format is read from the header record
(default 3.4).

## Tests

`make test` builds items exactly as Janus does and checks routing, source-id
rewriting, byte identity, the trigger-id offsets for every record layout and
data format 3.3/3.4, timing mode, the 32-byte timing-event/header-record
ambiguity, body-header insertion for header-less items, body-header
extensions, masked and malformed input.  `./test_router --write run.evt`
writes the test stream as an event file usable as `file:///…/run.evt` source.
