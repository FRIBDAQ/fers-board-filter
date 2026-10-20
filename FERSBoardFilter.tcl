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
# @file FERSBoardFilter.tcl
# @brief ReadoutShell callout bundle that runs fersBoardFilter.
#
# Usage in ReadoutCallouts.tcl (see the one in this directory):
#
#     lappend auto_path <this directory>
#     package require FERSBoardFilter
#     FERSBoardFilter::register        ;# before ::EVBC::useEventBuilder
#
# When a run starts (Halted -> Active) and the filter is not running yet,
# it is started with the settings of FERSBoardFilterSettings.tcl and its
# output is parsed by the Actions package into the ReadoutGUI log.  The
# filter keeps running between runs; if it exits it is restarted at the
# next run start.  Before starting, a producer left on an output ring by
# an earlier session is killed, and the start waits until every output
# ring exists so that the event builder's ring sources find them.

lappend auto_path [file join $::env(DAQROOT) TclLibs]

package provide FERSBoardFilter 1.0

package require Actions
package require DefaultActions
package require RunstateMachine
package require ReadoutGUIPanel
package require ring

namespace eval FERSBoardFilter {
    variable here [file dirname [file normalize [info script]]]
    variable pipe {}
    variable parser [::Actions %AUTO%]
    variable maxBoards 16

    # Settings
    variable inring
    variable boardMask
    variable timestamp
    source [file join $here FERSBoardFilterSettings.tcl]

    set ::DefaultActions::name FERSBoardFilter

    ##
    # Name of the ring Janus writes to (from the settings)
    proc inring {} {
        variable inring
        return $inring
    }

    ##
    # Active board numbers, from the settings' boardMask
    proc boards {} {
        variable boardMask
        variable maxBoards
        set result [list]
        for {set b 0} {$b < $maxBoards} {incr b} {
            if {$boardMask & (1 << $b)} {
                lappend result $b
            }
        }
        return $result
    }

    ##
    # Output ring names, one per active board
    proc outrings {} {
        variable inring
        set result [list]
        foreach b [boards] {
            lappend result ${inring}_$b
        }
        return $result
    }

    ##
    # Add this bundle to the run state machine
    proc register {} {
        if {[llength [boards]] == 0} {
            error "FERSBoardFilter: boardMask $FERSBoardFilter::boardMask keeps no board"
        }
        set stateMachine [RunstateMachineSingleton %AUTO%]
        $stateMachine addCalloutBundle FERSBoardFilter
        $stateMachine destroy
    }

    #--------------------------------------------------------------------
    # Callout bundle interface

    proc attach {state} {
    }

    proc enter {from to} {
    }

    proc leave {from to} {
        if {($from eq "Halted") && ($to eq "Active")} {
            start
        }
    }

    #--------------------------------------------------------------------
    # Private

    ##
    # Start the filter if it is not running, then wait for its rings
    proc start {} {
        variable here
        variable pipe
        variable parser
        variable inring
        variable boardMask
        variable timestamp

        if {$pipe eq {}} {
            set cmd [file join $here fersBoardFilter]
            if {![file executable $cmd]} {
                error "FERSBoardFilter: $cmd is not built; run make in $here"
            }

            foreach ring [outrings] {
                killOldProducer $ring
            }

            set pipe [open "| $cmd --timestamp=$timestamp $inring $boardMask |& cat" r]
            chan configure $pipe -blocking 0 -buffering line
            chan event $pipe readable [list FERSBoardFilter::onReadable]
            ReadoutGUIPanel::Log FERSBoardFilter log \
                "Started $cmd --timestamp=$timestamp $inring $boardMask (pid [pid $pipe])"
        }

        # Also when already running: the rings must be there before the
        # event builder's ring sources start
        waitForRings
    }

    ##
    # Pipe readable: hand the output to the Actions parser; on EOF forget
    # the pipe so that the next run start launches the filter again.
    proc onReadable {} {
        variable pipe
        variable parser

        if {$pipe eq {}} {
            return
        }
        set fd $pipe
        if {[eof $fd]} {
            set pipe {}
            ReadoutGUIPanel::Log FERSBoardFilter error \
                "fersBoardFilter exited; it will be started again at the next run start"
        }
        $parser onReadable $fd
    }

    ##
    # Kill the producer of an output ring left behind by an earlier session
    proc killOldProducer {ring} {
        if {$ring in [ringbuffer list]} {
            set producerPid [lindex [ringbuffer usage $ring] 3]
            if {$producerPid != -1} {
                ReadoutGUIPanel::Log FERSBoardFilter log \
                    "Killing the old producer of $ring (pid $producerPid)"
                catch {exec kill $producerPid}
            }
        }
    }

    ##
    # Wait (up to 5 s) until every output ring exists
    proc waitForRings {} {
        for {set i 0} {$i < 50} {incr i} {
            set missing [list]
            set rings [ringbuffer list]
            foreach ring [outrings] {
                if {$ring ni $rings} {
                    lappend missing $ring
                }
            }
            if {[llength $missing] == 0} {
                return
            }
            after 100
        }
        error "FERSBoardFilter: ring(s) $missing were not created by fersBoardFilter"
    }

    namespace export attach enter leave
}
