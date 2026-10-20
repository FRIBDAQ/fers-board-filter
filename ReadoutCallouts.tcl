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
# @brief Run fersBoardFilter from ReadoutShell and build events over the
#        per-board rings it makes.
#
# Janus writes every board's events into one ring.  The FERSBoardFilter
# package (this directory) starts fersBoardFilter when a run begins; it
# copies the events into ${inring}_<board> with source id <board>.  Each
# of those rings is registered here as one event-builder source, so glom
# assembles one built event per trigger and writes it to ${inring}_event.
#
# Settings (ring name, board mask, timestamp mode) are in
# FERSBoardFilterSettings.tcl next to this file.

set here [file dirname [file normalize [info script]]]
lappend auto_path [file join $::env(DAQROOT) TclLibs]
lappend auto_path $here

package require FERSBoardFilter
package require evbcallouts

# Coincidence window in body-header timestamp units.  Janus stamps items
# with tstamp_us*10000, i.e. 1 us = 10000.  With timestamp "trigger" in the
# settings the stamp is the trigger_id and dt 0 builds by trigger number.
set glomdt 10000

# The filter must be running (rings created) before the event builder's
# ring sources start, so register it ahead of the event builder.
FERSBoardFilter::register
::EVBC::useEventBuilder

proc OnStart {} {
    # -glomid: source id of the built events, outside the board ids
    EVBC::initialize -restart true -glombuild true -glomdt $::glomdt \
        -glomid 100 -destring [FERSBoardFilter::inring]_event
}

foreach board [FERSBoardFilter::boards] {
    # URL, no timestamp extractor (body headers carry the stamps), source id,
    # description, expect body headers, one END_RUN per run, 10 s end timeout
    EVBC::registerRingSource tcp://localhost/[FERSBoardFilter::inring]_$board \
        "" $board FERS_board_$board 1 1 10
}
