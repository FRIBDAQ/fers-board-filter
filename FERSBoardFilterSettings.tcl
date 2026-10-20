# Settings of the FERSBoardFilter package (sourced by FERSBoardFilter.tcl).

# Ring Janus writes to ("Output RingBuffer" of the ReadoutShell Janus
# provider / RingBufferName in the Janus config).  Board b's events go to
# ${inring}_b, built events to ${inring}_event.
set inring    input

# Active boards, bit b set = board b (0x3: boards 0 and 1).
set boardMask 0x3

# "keep": body-header timestamps as Janus assigns them (tstamp_us*10000);
# "trigger": the event's trigger_id (build with glomdt 0; not in timing mode).
set timestamp keep
