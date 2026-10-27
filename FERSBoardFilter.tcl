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
# next run start.  Before starting, a producer left on the output ring by
# an earlier session is killed, and the run start waits until the filter
# produces into the output ring ${inring}_event.

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
    variable key
    variable dt
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
    # Ring the built events go to
    proc outring {} {
        variable inring
        return ${inring}_event
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
    # Start the filter if it is not running, then wait until it produces
    # into every output ring
    proc start {} {
        variable here
        variable pipe
        variable parser
        variable inring
        variable boardMask
        variable key
        variable dt

        if {$pipe eq {}} {
            set cmd [file join $here fersBoardFilter]
            if {![file executable $cmd]} {
                error "FERSBoardFilter: $cmd is not built; run make in $here"
            }

            freeProducer [outring]

            set args "--key=$key --dt=$dt $inring $boardMask"
            set pipe [open "| $cmd $args |& cat" r]
            chan configure $pipe -blocking 0 -buffering line
            chan event $pipe readable [list FERSBoardFilter::onReadable]
            ReadoutGUIPanel::Log FERSBoardFilter log \
                "Started $cmd $args (pid [pid $pipe])"
        }

        # Also when already running: a filter that could not attach (or has
        # died) must stop the run start here instead of leaving the output
        # ring without data
        waitForProducer [lindex [pid $pipe] 0]
    }

    ##
    # Pipe readable: hand the output to the Actions parser.  On EOF (all
    # output is parsed by then) close the pipe here rather than in the
    # parser, which pops up a dialog, so that the exit status can be
    # logged, and forget it so that the next run start launches the filter
    # again.
    proc onReadable {} {
        variable pipe
        variable parser

        if {$pipe eq {}} {
            return
        }
        set fd $pipe
        if {![eof $fd]} {
            $parser onReadable $fd
            return
        }
        set pipe {}
        chan event $fd readable {}
        chan configure $fd -blocking 1
        set how exited
        if {[catch {close $fd} msg opts]} {
            set code [dict get $opts -errorcode]
            switch [lindex $code 0] {
                CHILDSTATUS { set how "exited with status [lindex $code 2]" }
                CHILDKILLED { set how "was killed by [lindex $code 2]" }
                default     { set how "exited ($msg)" }
            }
        }
        ReadoutGUIPanel::Log FERSBoardFilter error \
            "fersBoardFilter $how; it will be started again at the next run start"
    }

    ##
    # Producer pid of a ring; -1 when it has none or does not exist
    proc producerOf {ring} {
        if {[catch {lindex [ringbuffer usage $ring] 3} producerPid]} {
            return -1
        }
        return $producerPid
    }

    ##
    # True while process p runs (a zombie, i.e. an exited child not yet
    # reaped, counts as gone)
    proc isAlive {p} {
        if {[catch {open /proc/$p/stat} fd]} {
            return 0
        }
        set stat [read $fd]
        close $fd
        # The state follows the parenthesized command name
        set state [string index [string trimleft [string range $stat [string last ")" $stat]+1 end]] 0]
        return [expr {$state ne "Z"}]
    }

    ##
    # Free the producer slot of the output ring left behind by an earlier
    # session.  The RingMaster releases a dead producer's slot only once it
    # notices the process is gone, and a filter started before that cannot
    # attach ("already has a producer"); so wait (up to 5 s) for the
    # release, and release the slot here once its process is dead.
    proc freeProducer {ring} {
        set producerPid [producerOf $ring]
        if {$producerPid == -1} {
            return
        }
        if {[isAlive $producerPid]} {
            ReadoutGUIPanel::Log FERSBoardFilter log \
                "Killing the old producer of $ring (pid $producerPid)"
            catch {exec kill $producerPid}
        }
        for {set i 0} {$i < 50} {incr i} {
            if {[producerOf $ring] == -1} {
                return
            }
            if {![isAlive $producerPid]} {
                catch {ringbuffer disconnect producer $ring}
            }
            after 100
        }
        error "FERSBoardFilter: the producer of $ring (pid $producerPid) does not go away"
    }

    ##
    # Wait (up to 5 s) until the filter is the producer of the output ring
    proc waitForProducer {filterPid} {
        for {set i 0} {$i < 50} {incr i} {
            if {[producerOf [outring]] == $filterPid} {
                return
            }
            if {![isAlive $filterPid]} {
                error "FERSBoardFilter: fersBoardFilter exited; its error is in the log"
            }
            after 100
        }
        error "FERSBoardFilter: fersBoardFilter (pid $filterPid) is not the producer of [outring]"
    }

    namespace export attach enter leave
}
