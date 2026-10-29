# editor.tcl --
#
# This demonstration script is an editor of scripts of one's own, which runs
# them in the interpreter of the demos, as "Rerun Demo" does, and shows what
# they print and their errors.

if {![info exists widgetDemo]} {
    error "This script should be run from the \"widget\" demo."
}

package require tk

set w .editor
if {[winfo exists $w]} {
    # Keep the script being edited.
    wm deiconify $w
    raise $w
    return
}
toplevel $w
wm title $w "Your Own Script"
wm iconname $w "editor"
positionWindow $w

label $w.msg -font $font -wraplength 4i -anchor w -justify left -text "Write a\
	script, or open one, and press Run. Like the demos, it runs in this\
	application: give it a toplevel of its own, which it makes again when\
	it runs again. What it prints, and its errors, are shown below the\
	script."
bind $w.msg <Configure> {%W configure -wraplength [expr {%w - 4}]}
pack $w.msg -side top -fill x -padx 3p -pady 3p

## See Code / Dismiss buttons
set btns [addSeeDismiss $w.buttons $w]
pack $btns -side bottom -fill x

set f [ttk::frame $w.tools]
ttk::button $f.run -text "Run" -command [list editorRun $w]
ttk::button $f.open -text "Open..." -command [list editorOpen $w]
ttk::button $f.save -text "Save..." -command [list editorSave $w]
ttk::button $f.clear -text "Clear Output" \
	-command [list editorClear $w.pane.out.text]
pack $f.run $f.open $f.save $f.clear -side left -padx 3p -pady 3p
pack $f -side top -fill x

ttk::panedwindow $w.pane -orient vertical
pack $w.pane -side top -fill both -expand 1 -padx 3p -pady 3p
foreach {pane height} {script 16 out 5} {
    set p [ttk::frame $w.pane.$pane]
    text $p.text -font fixedFont -height $height -width 60 -wrap none \
	    -undo 1 -yscrollcommand [list $p.yscroll set] \
	    -xscrollcommand [list $p.xscroll set]
    ttk::scrollbar $p.yscroll -command [list $p.text yview]
    ttk::scrollbar $p.xscroll -orient horizontal \
	    -command [list $p.text xview]
    grid $p.text $p.yscroll -sticky news
    grid $p.xscroll -sticky ew
    grid rowconfigure $p 0 -weight 1
    grid columnconfigure $p 0 -weight 1
    $w.pane add $p -weight [expr {$height > 10 ? 3 : 1}]
}
$w.pane.out.text tag configure stderr -foreground red3
$w.pane.out.text configure -state disabled

$w.pane.script.text insert 1.0 {set w .myscript
destroy $w
toplevel $w
wm title $w "My Script"

ttk::label $w.l -text "Hello, world!" -font {Helvetica 18}
ttk::button $w.b -text "Print the time" -command {
    puts "It is [clock format [clock seconds] -format %T]"
}
ttk::button $w.d -text "Dismiss" -command [list destroy $w]
pack $w.l $w.b $w.d -padx 10p -pady 5p
}
$w.pane.script.text edit reset
$w.pane.script.text mark set insert 1.0

# editorCapture --
# The transformation of stdout and stderr, which shows what is written on
# them in the output of the editor, and writes it through.
#
# Arguments:
# out -		The text of the output.
# chan -	stdout or stderr.
# op -		The operation.
# handle -	The handle of the transformation.
# args -	The data, for a write.

proc editorCapture {out chan op handle args} {
    switch -- $op {
	initialize {
	    return {initialize finalize write}
	}
	write {
	    set data [lindex $args 0]
	    if {[winfo exists $out]} {
		$out configure -state normal
		$out insert end [encoding convertfrom -profile replace \
			[chan configure $chan -encoding] $data] $chan
		$out configure -state disabled
		$out see end
	    }
	    return $data
	}
    }
}

# editorRun --
# Run the script of the editor in the global namespace, and show the error
# it raises, if any.
#
# Arguments:
# w -		The editor.

proc editorRun {w} {
    set script [$w.pane.script.text get 1.0 end-1c]
    if {[catch {uplevel #0 $script} msg opts]} {
	# Without the lines of the "uplevel" of this procedure.
	puts stderr [join [lrange [split [dict get $opts -errorinfo] \n] \
		0 end-3] \n]
    }
}

# editorClear --
# Clear the output of the editor.
#
# Arguments:
# out -		The text of the output.

proc editorClear {out} {
    $out configure -state normal
    $out delete 1.0 end
    $out configure -state disabled
}

# editorOpen --
# Replace the script of the editor with the contents of a file.
#
# Arguments:
# w -		The editor.

proc editorOpen {w} {
    set types {{"Tcl Scripts" .tcl} {"All files" *}}
    set file [tk_getOpenFile -parent $w -filetypes $types \
	    -initialdir $::env(HOME)]
    if {$file eq ""} {
	return
    }
    if {[catch {
	set f [open $file]
	set script [read $f]
	close $f
    } msg]} {
	tk_messageBox -parent $w -icon error -message $msg
	return
    }
    $w.pane.script.text delete 1.0 end
    $w.pane.script.text insert 1.0 $script
    $w.pane.script.text edit reset
    $w.pane.script.text mark set insert 1.0
    wm title $w "Your Own Script: [file tail $file]"
}

# editorSave --
# Save the script of the editor in a file.
#
# Arguments:
# w -		The editor.

proc editorSave {w} {
    set types {{"Tcl Scripts" .tcl} {"All files" *}}
    set file [tk_getSaveFile -parent $w -filetypes $types \
	    -initialdir $::env(HOME) -defaultextension .tcl]
    if {$file eq ""} {
	return
    }
    if {[catch {
	set f [open $file w]
	puts -nonewline $f [$w.pane.script.text get 1.0 end-1c]
	close $f
    } msg]} {
	tk_messageBox -parent $w -icon error -message $msg
	return
    }
    wm title $w "Your Own Script: [file tail $file]"
}

# What is written on stdout and stderr is shown, line by line, while the
# editor exists.

foreach chan {stdout stderr} {
    chan push $chan [list editorCapture $w.pane.out.text $chan]
    chan configure $chan -buffering line
}
bind $w.pane <Destroy> {
    catch {chan pop stdout}
    catch {chan pop stderr}
}
