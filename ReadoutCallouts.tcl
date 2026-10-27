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
# @brief Run fersBoardFilter from ReadoutShell.
#
# Janus writes every board's events into one ring.  The FERSBoardFilter
# package (this directory) starts fersBoardFilter when a run begins; it
# builds the boards' items of one trigger into one event and writes the
# built events to ${inring}_event, where the event logger and SpecTcl read
# them.  No event builder is needed.
#
# Settings (ring name, board mask, key, window) are in
# FERSBoardFilterSettings.tcl next to this file.

set here [file dirname [file normalize [info script]]]
lappend auto_path [file join $::env(DAQROOT) TclLibs]
lappend auto_path $here

package require FERSBoardFilter

FERSBoardFilter::register
