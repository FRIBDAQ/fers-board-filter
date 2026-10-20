#    This software is Copyright by the Board of Trustees of Michigan
#    State University (c) Copyright 2026.
#
#    You may use this software under the terms of the GNU public license
#    (GPL).  The terms of this license are described at:
#
#     http://www.gnu.org/licenses/gpl.txt
#
#     Author:
#            Genie Jhang <changj@frib.msu.edu>
#      FRIB
#      Michigan State University
#      East Lansing, MI 48824-1321

##
# @file ReadoutCallouts.tcl
# @brief Event builder over the per-board rings made by fersBoardFilter.
#
# Janus writes every board's events into one ring ($inring).  Running
#
#     fersBoardFilter $inring $boardMask
#
# next to Janus copies them into ${inring}_<board> with source id <board>.
# Each of those rings is registered here as one event-builder source, so
# glom assembles one built event per trigger from all active boards and
# writes it to ${inring}_event.  Edit the three settings below.

package require evbcallouts

# Ring Janus writes to (same name as given to fersBoardFilter)
set inring    input
# Active boards, bit b set = board b (same mask as given to fersBoardFilter)
set boardMask 0x3
# Coincidence window in body-header timestamp units.  Janus stamps items with
# tstamp_us*10000, i.e. 1 us = 10000.  With "fersBoardFilter --timestamp=trigger"
# the stamp is the trigger_id and dt 0 builds by trigger number.
set glomdt    10000

::EVBC::useEventBuilder

proc OnStart {} {
    EVBC::initialize -restart true -glombuild true -glomdt $::glomdt \
        -destring ${::inring}_event
}

for {set board 0} {$board < 8} {incr board} {
    if {$::boardMask & (1 << $board)} {
        # URL, no timestamp extractor (body headers carry the stamps), source id,
        # description, expect body headers, one END_RUN per run, 10 s end timeout
        EVBC::registerRingSource tcp://localhost/${inring}_$board "" $board \
            "FERS board $board" 1 1 10
    }
}
