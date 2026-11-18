# runtests.tcl --
#
#	The application of the APK of the tests ("make test-apk"): run the
#	tests of Tk and Ttk in the directory tests next to it with the options
#	of tcltest in the file testflags, in a single process, and exit. The
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

# Each suite (tests/all.tcl, then tests/ttk/all.tcl, which all.tcl leaves
# out) exits at its end: exit goes on with the next suite, and once the last
# has run, writes tktest.done and exits.
rename exit ::tktest::Exit
proc exit {{status 0}} {
    catch {
	flush [tcltest::outputChannel]
	flush [tcltest::errorChannel]
    }
    if {$::tktest::running} {
	return -code error -errorcode {TKTEST EXIT} $status
    }
    catch {close [open $::tktest::log.done w]}
    ::tktest::Exit $status
}

namespace eval ::tktest {
    variable running 1
    variable suite
    variable flags
}
if {[catch {
    set f [open [file join $::tktest::dir testflags]]
    set ::tktest::flags [string trim [read $f]]
    close $f
} msg]} {
    set f [open $::tktest::log.err a]
    puts $f $::errorInfo
    close $f
    set ::tktest::running 0
    exit
}
foreach ::tktest::suite {all.tcl ttk/all.tcl} {
    set argv [list -tmpdir $env(TMPDIR) -outfile $::tktest::log.log \
	    -errfile $::tktest::log.err {*}$::tktest::flags]
    set argc [llength $argv]
    set argv0 [file join $::tktest::dir tests $::tktest::suite]

    # all.tcl replaces tcltest::cleanupTests with one which sets the
    # interactive constraint at the end of the suite, so as not to exit
    # there; the next suite replaces it again.
    if {[info commands ::tcltest::CleanupTestsAndExit] ne ""} {
	rename ::tcltest::cleanupTests {}
	rename ::tcltest::CleanupTestsAndExit ::tcltest::cleanupTests
	::tcltest::testConstraint interactive 0
    }
    if {[catch {uplevel #0 [list source $argv0]} msg opts]
	    && [dict get $opts -errorcode] ne {TKTEST EXIT}} {
	set f [open $::tktest::log.err a]
	puts $f [dict get $opts -errorinfo]
	close $f
    }
}
set ::tktest::running 0
exit
