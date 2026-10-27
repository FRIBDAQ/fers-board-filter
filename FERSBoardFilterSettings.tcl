# Settings of the FERSBoardFilter package (sourced by FERSBoardFilter.tcl).

# Ring Janus writes to ("Output RingBuffer" of the ReadoutShell Janus
# provider / RingBufferName in the Janus config).  The built events go to
# ${inring}_event.
set inring    janus

# Active boards, bit b set = board b (0x3f: boards 0 to 5).
set boardMask 0x3f

# How the boards' items of one trigger are recognized:
#   time     they share the Janus timestamp within dt (the boards trigger
#            on their own, trigger_id is each board's own count)
#   trigger  they share the trigger_id (boards counting a common trigger)
set key       time

# Window for key time, in body-header timestamp units of the Janus build
# (this Janus writes tstamp_us * 10; the glomdt used before).
set dt        100
