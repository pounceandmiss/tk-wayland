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

# The working directory of an application is /, which it cannot read.
cd $env(TMPDIR)
set dir [file dirname [info script]]
set log [file join $env(HOME) tktest]
file delete $log.log $log.err $log.done

# tcltest exits at the end of the tests in Tk; tktest.done is written then.
rename exit tktestExit
proc exit {{status 0}} {
    catch {
	flush [tcltest::outputChannel]
	flush [tcltest::errorChannel]
    }
    close [open $::log.done w]
    tktestExit $status
}

if {[catch {
    set f [open [file join $dir testflags]]
    set flags [string trim [read $f]]
    close $f
    set argv [list -tmpdir $env(TMPDIR) -outfile $log.log -errfile $log.err \
	    {*}$flags]
    set argc [llength $argv]
    set argv0 [file join $dir tests all.tcl]
    source $argv0
}]} {
    set f [open $log.err a]
    puts $f $::errorInfo
    close $f
}
exit
