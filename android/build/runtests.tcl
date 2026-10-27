# runtests.tcl --
#
#	The application of the APK of the tests ("make test-apk"): run the
#	tests of Tk in the directory tests next to it with the options of
#	tcltest in the file testflags, in a single process, and exit. The
#	output goes to tktest.log and tktest.err in the home directory of the
#	application, which test.sh reads once tktest.done is there.
#
# Copyright © 2026 The Tk developers.
#
# See the file "license.terms" for information on usage and redistribution
# of this file, and for a DISCLAIMER OF ALL WARRANTIES.

# The working directory of an application is /, which it cannot read. The
# variables are in a namespace of their own, since the tests use globals.
cd $env(TMPDIR)
namespace eval ::tktest {
    variable dir [file dirname [info script]]
    variable log [file join $::env(HOME) tktest]
    file delete $log.log $log.err $log.done
}

# tcltest exits at the end of the tests in Tk; tktest.done is written then.
rename exit ::tktest::Exit
proc exit {{status 0}} {
    catch {
	flush [tcltest::outputChannel]
	flush [tcltest::errorChannel]
    }
    catch {close [open $::tktest::log.done w]}
    ::tktest::Exit $status
}

if {[catch {
    set f [open [file join $::tktest::dir testflags]]
    set flags [string trim [read $f]]
    close $f
    set argv [list -tmpdir $env(TMPDIR) -outfile $::tktest::log.log \
	    -errfile $::tktest::log.err {*}$flags]
    set argc [llength $argv]
    set argv0 [file join $::tktest::dir tests all.tcl]
    source $argv0
}]} {
    set f [open $::tktest::log.err a]
    puts $f $::errorInfo
    close $f
}
exit
