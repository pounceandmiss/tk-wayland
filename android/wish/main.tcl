# main.tcl --
#
#	Wish for Android: an editor of Tcl scripts, which runs them, and a
#	console. Each run of a script has an interpreter of its own, with Tk,
#	whose main window is shown above the editor (the back button closes
#	it); the console evaluates commands in the interpreter of the last
#	run. What the scripts write on stdout and stderr is shown in the
#	console. The scripts are saved in the directory "scripts" of the home
#	directory of the application, and the text of the editor is kept
#	across restarts.
#
# Copyright © 2026 The Tk developers.
#
# See the file "license.terms" for information on usage and redistribution of
# this file, and for a DISCLAIMER OF ALL WARRANTIES.

package require Tk

namespace eval ::wish {
    variable dir [file dirname [file normalize [info script]]]
    variable scripts [file join $::env(HOME) scripts]
    variable state [file join $::env(HOME) wish.state]
    variable file ""		;# The file being edited, if any.
    variable interp ""		;# The interpreter of the last run.
    variable count 0		;# The number of interpreters created.
    variable history {}		;# The commands of the console.
    variable historyPos 0
}

# ::wish::Init --
#
#	Build the window: a bar of buttons, and a notebook with the editor and
#	the console.
#
# Arguments:
#	None.
#
# Results:
#	None.

proc ::wish::Init {} {
    variable dir
    variable scripts

    file mkdir $scripts
    wm title . Wish
    wm protocol . WM_DELETE_WINDOW ::wish::Quit

    ttk::frame .bar
    ttk::button .bar.run -text Run -command ::wish::Run
    ttk::menubutton .bar.file -text File -menu .bar.file.m
    ttk::menubutton .bar.examples -text Examples -menu .bar.examples.m
    pack .bar.run .bar.file .bar.examples -side left -padx 2 -pady 2

    menu .bar.file.m -tearoff 0
    .bar.file.m add command -label New -command ::wish::New
    .bar.file.m add command -label Open... -command ::wish::Open
    .bar.file.m add command -label Save -command ::wish::Save
    .bar.file.m add command -label "Save As..." -command {::wish::Save 1}
    .bar.file.m add separator
    .bar.file.m add command -label Quit -command ::wish::Quit

    menu .bar.examples.m -tearoff 0
    foreach path [lsort [glob -nocomplain -directory \
	    [file join $dir examples] *.tcl]] {
	.bar.examples.m add command -label [file rootname [file tail $path]] \
		-command [list ::wish::LoadExample $path]
    }
    set widget [file join $dir demos widget]
    if {[file exists $widget]} {
	.bar.examples.m add separator
	.bar.examples.m add command -label "Widget tour" \
		-command [list ::wish::RunFile $widget]
    }

    ttk::notebook .nb
    ttk::frame .nb.script
    text .nb.script.text -wrap none -undo 1 -font TkFixedFont \
	    -yscrollcommand {.nb.script.y set} \
	    -xscrollcommand {.nb.script.x set}
    ttk::scrollbar .nb.script.y -command {.nb.script.text yview}
    ttk::scrollbar .nb.script.x -orient horizontal \
	    -command {.nb.script.text xview}
    grid .nb.script.text .nb.script.y -sticky nsew
    grid .nb.script.x -sticky ew
    grid rowconfigure .nb.script 0 -weight 1
    grid columnconfigure .nb.script 0 -weight 1
    bind .nb.script.text <<Modified>> ::wish::Modified

    ttk::frame .nb.console
    text .nb.console.text -wrap char -font TkFixedFont -height 10 \
	    -yscrollcommand {.nb.console.y set}
    ttk::scrollbar .nb.console.y -command {.nb.console.text yview}
    ttk::entry .nb.console.entry -font TkFixedFont
    grid .nb.console.text .nb.console.y -sticky nsew
    grid .nb.console.entry - -sticky ew
    grid rowconfigure .nb.console 0 -weight 1
    grid columnconfigure .nb.console 0 -weight 1
    .nb.console.text tag configure command -foreground blue4
    .nb.console.text tag configure stderr -foreground red3
    .nb.console.text tag configure error -foreground red3
    .nb.console.text tag configure info -foreground gray40
    .nb.console.text configure -state disabled
    bind .nb.console.entry <Return> ::wish::Command
    bind .nb.console.entry <Up> {::wish::History -1}
    bind .nb.console.entry <Down> {::wish::History 1}

    .nb add .nb.script -text Script
    .nb add .nb.console -text Console
    pack .bar -side top -fill x
    pack .nb -side top -fill both -expand 1

    foreach chan {stdout stderr} {
	chan push $chan [list ::wish::Capture $chan]
	chan configure $chan -buffering none
    }
    Restore
    Print "Wish [info patchlevel]: the console evaluates commands in the\
	    interpreter of the last run.\n" info
}

# ::wish::Capture --
#
#	The transformation of stdout and stderr, which shows what is written
#	on them in the console, and writes it through.
#
# Arguments:
#	chan	stdout or stderr.
#	op	The operation.
#	handle	The handle of the transformation.
#	args	The data, for a write.
#
# Results:
#	As with "chan push".

proc ::wish::Capture {chan op handle args} {
    switch -- $op {
	initialize {
	    return {initialize finalize write}
	}
	write {
	    set data [lindex $args 0]
	    Print [encoding convertfrom -profile replace \
		    [chan configure $chan -encoding] $data] $chan
	    return $data
	}
    }
}

# ::wish::Print --
#
#	Show text in the console.
#
# Arguments:
#	text	The text.
#	tag	The tag of the text in the console: stdout, stderr, command,
#		error or info.
#
# Results:
#	None.

proc ::wish::Print {text {tag stdout}} {
    if {![winfo exists .nb.console.text]} {
	return
    }
    .nb.console.text configure -state normal
    .nb.console.text insert end $text $tag
    .nb.console.text configure -state disabled
    .nb.console.text see end
}

# ::wish::NewInterp --
#
#	Create the interpreter of a run, with Tk, after deleting the one of
#	the last run. Its main window is withdrawn until it has children, and
#	"exit" deletes it.
#
# Arguments:
#	name	The name of the script.
#
# Results:
#	The interpreter.

proc ::wish::NewInterp {name} {
    variable interp
    variable count

    Delete
    set interp wish[incr count]
    interp create $interp
    $interp eval [list set argv0 $name]
    $interp eval {set argv {}; set argc 0}
    load {} Tk $interp
    $interp eval [list wm title . [file tail $name]]
    $interp eval {wm withdraw .}
    interp alias $interp exit {} ::wish::Exit $interp
    interp alias $interp ::wish::Closed {} ::wish::Closed $interp
    interp alias $interp ::wish::BgError {} ::wish::BgError
    interp bgerror $interp ::wish::BgError
    $interp eval {bind . <Destroy> {if {"%W" eq "."} ::wish::Closed}}
    return $interp
}

# ::wish::Delete --
#
#	Delete the interpreter of the last run, and its windows.
#
# Arguments:
#	None.
#
# Results:
#	None.

proc ::wish::Delete {} {
    variable interp

    if {$interp ne "" && [interp exists $interp]} {
	interp delete $interp
    }
    set interp ""
}

# ::wish::Exit --
#
#	"exit" in the interpreter of a run: delete it, once it no longer
#	runs.
#
# Arguments:
#	i	The interpreter.
#	args	The code of "exit", ignored.
#
# Results:
#	None.

proc ::wish::Exit {i args} {
    after idle [list ::wish::Closed $i]
}

# ::wish::Closed --
#
#	The main window of the interpreter of a run was destroyed, or the run
#	exited: delete the interpreter.
#
# Arguments:
#	i	The interpreter.
#
# Results:
#	None.

proc ::wish::Closed {i} {
    variable interp

    if {$i eq $interp} {
	after idle ::wish::Delete
	Print "--- [$i eval {set argv0}] ended\n" info
    }
}

# ::wish::BgError --
#
#	Show a background error of a run in the console.
#
# Arguments:
#	msg	The message of the error.
#	opts	Its options.
#
# Results:
#	None.

proc ::wish::BgError {msg opts} {
    Print "[dict get $opts -errorinfo]\n" error
    .nb select .nb.console
}

# ::wish::Eval --
#
#	Run a script in the interpreter of a run, and show its main window if
#	it has children.
#
# Arguments:
#	i	The interpreter.
#	script	The script.
#
# Results:
#	None.

proc ::wish::Eval {i script} {
    if {![interp exists $i]} {
	return
    }
    if {[catch {$i eval $script} msg opts]} {
	Print "[dict get $opts -errorinfo]\n" error
	.nb select .nb.console
    }
    if {[interp exists $i] && [$i eval {winfo exists .}]
	    && [llength [$i eval {winfo children .}]]
	    && [$i eval {wm state .}] eq "withdrawn"} {
	$i eval {wm deiconify .}
    }
}

# ::wish::Run --
#
#	Run the script of the editor, in a new interpreter.
#
# Arguments:
#	None.
#
# Results:
#	None.

proc ::wish::Run {} {
    variable file

    SaveState
    set name [expr {$file eq "" ? "untitled" : [file tail $file]}]
    set i [NewInterp $name]
    Print "--- $name\n" info
    after idle [list ::wish::Eval $i [.nb.script.text get 1.0 end-1c]]
}

# ::wish::RunFile --
#
#	Run a script file, in a new interpreter.
#
# Arguments:
#	path	The file.
#
# Results:
#	None.

proc ::wish::RunFile {path} {
    set i [NewInterp $path]
    Print "--- [file tail $path]\n" info
    after idle [list ::wish::Eval $i [list source $path]]
}

# ::wish::Command --
#
#	Evaluate the command of the entry of the console, in the interpreter
#	of the last run, or a new one if there is none.
#
# Arguments:
#	None.
#
# Results:
#	None.

proc ::wish::Command {} {
    variable interp
    variable history
    variable historyPos

    set command [.nb.console.entry get]
    .nb.console.entry delete 0 end
    if {[string trim $command] eq ""} {
	return
    }
    if {$command ne [lindex $history end]} {
	lappend history $command
    }
    set historyPos [llength $history]
    Print "% $command\n" command
    if {$interp eq "" || ![interp exists $interp]} {
	NewInterp console
    }
    if {[catch {$interp eval $command} msg]} {
	Print "$msg\n" error
    } elseif {$msg ne ""} {
	Print "$msg\n"
    }
}

# ::wish::History --
#
#	Move in the history of the commands of the console.
#
# Arguments:
#	dir	-1 for the previous command, 1 for the next one.
#
# Results:
#	None.

proc ::wish::History {dir} {
    variable history
    variable historyPos

    set pos [expr {$historyPos + $dir}]
    if {$pos < 0 || $pos > [llength $history]} {
	return
    }
    set historyPos $pos
    .nb.console.entry delete 0 end
    .nb.console.entry insert 0 [lindex $history $pos]
    .nb.console.entry icursor end
}

# ::wish::SetText --
#
#	Replace the text of the editor.
#
# Arguments:
#	text	The text.
#	path	The file it is from, or "".
#
# Results:
#	None.

proc ::wish::SetText {text path} {
    variable file

    .nb.script.text delete 1.0 end
    .nb.script.text insert 1.0 $text
    .nb.script.text edit reset
    .nb.script.text edit modified 0
    .nb.script.text mark set insert 1.0
    set file $path
    Title
    .nb select .nb.script
}

# ::wish::Title --
#
#	Set the title of the main window from the file being edited.
#
# Arguments:
#	None.
#
# Results:
#	None.

proc ::wish::Title {} {
    variable file

    set name [expr {$file eq "" ? "untitled" : [file tail $file]}]
    if {[.nb.script.text edit modified]} {
	append name *
    }
    wm title . "Wish - $name"
}

# ::wish::Modified --
#
#	The text of the editor was changed: update the title, and save the
#	state soon (the application may be stopped at any time).
#
# Arguments:
#	None.
#
# Results:
#	None.

proc ::wish::Modified {} {
    Title
    after cancel ::wish::SaveState
    after 2000 ::wish::SaveState
}

# ::wish::Discard --
#
#	Ask whether to save the changes of the editor before they are
#	replaced.
#
# Arguments:
#	None.
#
# Results:
#	1 if the text may be replaced, 0 if the user cancelled.

proc ::wish::Discard {} {
    variable file

    if {![.nb.script.text edit modified]} {
	return 1
    }
    set name [expr {$file eq "" ? "untitled" : [file tail $file]}]
    switch -- [tk_messageBox -parent . -type yesnocancel -icon question \
	    -message "Save the changes to $name?"] {
	yes {
	    return [Save]
	}
	no {
	    return 1
	}
	default {
	    return 0
	}
    }
}

# ::wish::New, ::wish::Open, ::wish::LoadExample --
#
#	Edit a new script, a script file, or a copy of an example.
#
# Arguments:
#	path	The example (::wish::LoadExample).
#
# Results:
#	None.

proc ::wish::New {} {
    if {[Discard]} {
	SetText "" ""
    }
}

proc ::wish::Open {} {
    variable scripts

    if {![Discard]} {
	return
    }
    set path [tk_getOpenFile -parent . -initialdir $scripts \
	    -filetypes {{{Tcl scripts} .tcl} {{All files} *}}]
    if {$path eq ""} {
	return
    }
    if {[catch {
	set f [open $path]
	set text [read $f]
	close $f
    } msg]} {
	tk_messageBox -parent . -icon error -message $msg
	return
    }
    SetText $text $path
}

proc ::wish::LoadExample {path} {
    if {![Discard]} {
	return
    }
    set f [open $path]
    SetText [read $f] ""
    close $f
}

# ::wish::Save --
#
#	Save the script of the editor in its file, or in a file chosen.
#
# Arguments:
#	as	1 to choose the file even if there is one.
#
# Results:
#	1 if the script was saved, 0 if not.

proc ::wish::Save {{as 0}} {
    variable file
    variable scripts

    set path $file
    if {$as || $path eq ""} {
	set path [tk_getSaveFile -parent . -initialdir $scripts \
		-defaultextension .tcl \
		-filetypes {{{Tcl scripts} .tcl} {{All files} *}}]
	if {$path eq ""} {
	    return 0
	}
    }
    if {[catch {
	set f [open $path w]
	puts -nonewline $f [.nb.script.text get 1.0 end-1c]
	close $f
    } msg]} {
	tk_messageBox -parent . -icon error -message $msg
	return 0
    }
    set file $path
    .nb.script.text edit modified 0
    Title
    SaveState
    return 1
}

# ::wish::SaveState, ::wish::Restore --
#
#	Save and restore the text of the editor, its file and whether it was
#	modified, and the history of the console.
#
# Arguments:
#	None.
#
# Results:
#	None.

proc ::wish::SaveState {} {
    variable state
    variable file
    variable history

    after cancel ::wish::SaveState
    catch {
	set f [open $state.new w]
	puts -nonewline $f [dict create file $file \
		modified [.nb.script.text edit modified] \
		text [.nb.script.text get 1.0 end-1c] \
		history [lrange $history end-99 end]]
	close $f
	file rename -force $state.new $state
    }
}

proc ::wish::Restore {} {
    variable state
    variable dir
    variable history
    variable historyPos

    if {[catch {
	set f [open $state]
	set saved [read $f]
	close $f
	SetText [dict get $saved text] [dict get $saved file]
	.nb.script.text edit modified [dict get $saved modified]
	set history [dict get $saved history]
	set historyPos [llength $history]
	Title
    }]} {
	set f [open [file join $dir examples hello.tcl]]
	SetText [read $f] ""
	close $f
    }
}

# ::wish::Quit --
#
#	Save the state and exit.
#
# Arguments:
#	None.
#
# Results:
#	None.

proc ::wish::Quit {} {
    SaveState
    exit
}

::wish::Init
