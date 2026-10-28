# fersBoardFilter

Builds the ring items Janus (janus-frib) writes – one `PHYSICS_EVENT` per FERS
board and trigger – into one event per trigger, the way the NSCLDAQ event
builder would from one ring source per board, and writes the built events to
one ring.  No event builder, no per-board rings.

Janus writes every board's items with the same body-header source id, and
they arrive in bunches (a run of one board's triggers, then the next board's)
rather than trigger by trigger.  `fersBoardFilter` reads that ring and

* groups the board items of a trigger by a key: the Janus timestamp within a
  window `dt` (`--key=time`, the default; the boards trigger on their own and
  each board's `trigger_id` is its own count) or the `trigger_id` itself
  (`--key=trigger`, for boards counting a common trigger);
* releases a trigger, in key order, once every active board has contributed
  to it or has gone past it (sent a later key), at BEGIN/END_RUN, and – for a
  board that stopped sending – once more than `--max-pending` triggers are
  open (oldest first), so the output never waits for a dead board forever;
* writes it as one built `PHYSICS_EVENT` in the layout glom writes (below),
  one fragment per board item with the fragment and body-header source id set
  to the board number, fragments in timestamp order;
* passes BEGIN/END_RUN (and any other non-event item) through after releasing
  the open triggers, with the body-header source id set to its own, and
  writes the Janus file-header record (once per run) as a built event with
  that one fragment;
* starts its output with a `RING_FORMAT` item and follows every BEGIN_RUN
  with an `EVB_GLOM_INFO` item (window `dt`, timestamp policy), as glom
  does, so the stream looks like the event builder's to its consumers;
* creates the output ring `<inring>_event` – and a local input ring – when
  they do not exist, so it can be started before Janus;
* drops events of boards that are not in the mask (one notice per board) and
  malformed items (counted, reported at END_RUN).

SpecTcl-Janus reads the built events with its `unpackBuiltEvent` path, as it
did glom's; `boardMask` is not used there.  Run on a recorded Janus ring
(`janus.evt`, 6 boards, Janus 3.6.0) with `--dt=100` the output had the same
975,147 events with the same fragments as the event builder's `janus_event.evt`
(glom `dt 100`); only the order of fragments with identical timestamps can
differ, which is arbitrary in the orderer too.

## Files

| file | what |
|---|---|
| `fersBoardFilter.cpp`, `EventBuilder.{h,cpp}` | the filter program |
| `test_builder.cpp` | unit tests of the building (`make test`) |
| `FERSBoardFilter.tcl`, `pkgIndex.tcl` | ReadoutShell package that runs the filter |
| `FERSBoardFilterSettings.tcl` | ring name, board mask, key, window, source id, timestamp policy |
| `ReadoutCallouts.tcl` | runs the filter from ReadoutShell |

## Build

Needs an NSCLDAQ 12 installation (headers, `libdaqio`, `libdataformat`,
`libDataFlow`, `libdaqshm`, `liburl`, `libException`).

    source /usr/opt/nscldaq/12.x/daqsetup.bash   # sets DAQROOT
    make            # fersBoardFilter
    make test       # builder checks; needs only DataFormat.h

Start the filter from an environment where `daqsetup.bash` was sourced
(`LD_LIBRARY_PATH`): the libraries it links load further NSCLDAQ libraries.

## Running from ReadoutShell (recommended)

1. Edit `FERSBoardFilterSettings.tcl`: `inring` (the ring Janus writes to,
   "Output RingBuffer" of the Janus provider), `boardMask` (bit *b* set =
   board *b*), `key`, `dt`, `sourceId` and `timestamp` (see below).
2. Point ReadoutShell at `ReadoutCallouts.tcl` (or merge its three lines into
   yours).  The event logger and SpecTcl read `<inring>_event`.

`FERSBoardFilter.tcl` is a run-state callout bundle built on the NSCLDAQ
`Actions`/`DefaultActions` packages, the same way as the MDPP software-trigger
launcher: when a run starts (Halted → Active) and the filter is not running it

* frees the producer slot of the output ring if an earlier session left one
  (kills the process when it still runs, releases the slot when it is dead),
* starts `fersBoardFilter --key=<key> --dt=<dt> --sourceid=<sourceId>
  --timestamp=<timestamp>
  <inring> <boardMask>` from
  this directory with its output piped through an `Actions` parser, so the
  filter's messages appear in the ReadoutGUI log with their severity
  (the filter writes `LOGMSG`/`WRNMSG`/`ERRMSG <length> <text>` packets),
* waits (up to 5 s) until the filter is the producer of `<inring>_event`; a
  filter that exited or could not attach makes the run start fail.

The filter keeps running between runs and reports the run's counts at every
END_RUN (they start over at BEGIN_RUN); if it exits, the log says with which
status or signal, and it is started again at the next run start.

## Running by hand

    fersBoardFilter [options] INRING MASK

* `INRING` – the ring Janus writes to: a local ring name or a URL
  (`tcp://host/name`, `file:///path/run.evt`).  The output ring is named
  after the ring part of it (`<name>_event`, or `--outring=NAME`) and lives
  on the host the filter runs on.
* `MASK` – active boards, bit *b* set = board *b* built, e.g. `0x3f` for
  boards 0–5; up to 16 boards.  (Note: the SpecTcl `boardMask` tree variable
  has the opposite sense – a set bit masks a board out.)  Only boards that
  send data belong in the mask: a silent board delays every event by
  `--max-pending` triggers.
* `--key=time` (default): the items of a trigger share the body-header
  timestamp within `--dt`.  `--key=trigger`: they share the `trigger_id`
  (not possible in timing mode, whose events carry none: they are built by
  timestamp and the filter says so once; a spectiming run started mid-way,
  with no header record seen, is built by timestamp until the next run).
* `--dt=N` – the window, in body-header timestamp units, which depend on the
  Janus build: the Janus that recorded `janus.evt` writes `tstamp_us * 10`
  (100 ns), janus-frib 5.0.1 writes `tstamp_us * 10000` (100 ps).  Default
  100.  Boards of one trigger were within 3 units in `janus.evt`.
* `--max-pending=N` – open triggers before the oldest is released although a
  board has not gone past it (default 1000; about 450 bytes per board item).
* `--sourceid=N` – body-header source id of everything the filter writes:
  built events, the header-record event and the passed-through state
  changes (default 10).
* `--timestamp=first|last|average` – body-header timestamp of a built event:
  the earliest, the latest or the average of its fragments' (default last,
  glom's `--timestamp-policy`).

Example, Janus writing to ring `janus`, boards 0 to 5:

    fersBoardFilter janus 0x3f
    # -> ring janus_event

It exits only when a file source ends.  `CRingBuffer::put` blocks while the
output ring is full (a stalled consumer), which then also stalls Janus's ring;
with no consumer attached nothing blocks.  Attach online consumers that may
fall behind (SpecTcl) through a sampling `ringselector`.

## Output layout

A built event, as glom writes it (`unpackBuiltEvent` in SpecTcl-Janus walks
the same):

    RingItemHeader {size, PHYSICS_EVENT}
    BodyHeader {20, timestamp per --timestamp (default: latest fragment's),
                --sourceid, barrier 0}
    uint32 body bytes (itself included)
    fragments: uint64 timestamp | uint32 sourceId = board | uint32 bytes
               | uint32 barrier 0 | the board's ring item (body-header
               source id = board, otherwise byte-identical)

The header-record event has one fragment whose source id is the one Janus
gave the record.  State changes are the items Janus wrote with the
body-header source id replaced by `--sourceid` (one BEGIN_RUN and one
END_RUN per run, not one per board as the event builder emits).  So the body
header of every item the filter writes carries `--sourceid`; only the
fragments inside built events keep their own ids (the board number, or
Janus's for the header record).

The stream starts with a `RING_FORMAT` item (NSCLDAQ `FORMAT_MAJOR.MINOR`,
no body header) and every BEGIN_RUN is followed by an `EVB_GLOM_INFO` item
`{coincidenceTicks = dt (0 for --key=trigger), isBuilding 1, timestampPolicy
= --timestamp}`; glom writes the same two items, the second once per process.

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
them) and checks the built-event layout against glom's, bunched arrival,
release once every board has passed a trigger, a missed trigger, the
`--max-pending` cap and the flush at a state change, the window (joining
earlier and later within `dt`, a board twice), building by `trigger_id` in
every record layout and data format 3.3/3.4, timing mode, the 32-byte
timing-event/header-record ambiguity, header-less items, body-header
extensions, masked and malformed input.  `./test_builder --write run.evt`
writes the test stream as an event file usable as a `file:///…` source.
