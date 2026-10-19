# touch.tcl --
#
# This demonstration script shows what Tk does on touchscreens: the
# on-screen keyboard ("tk keyboard"), gestures ("tk touch" and <<Pinch>>),
# and the selection of text with handles.

if {![info exists widgetDemo]} {
    error "This script should be run from the \"widget\" demo."
}

package require tk

set w .touch
catch {destroy $w}
toplevel $w
wm title $w "Touchscreens"
wm iconname $w "touch"
positionWindow $w

label $w.msg -font $font -wraplength 4i -anchor w -justify left -text \
	"Tk turns touches into the events widgets know: a tap is a click,\
	dragging scrolls, a long press is a click of the third button, and\
	pinching sends <<Pinch>> events. The on-screen keyboard follows the\
	focus. The tabs below show these on a touchscreen; some of it can also\
	be tried with a mouse."
bind $w.msg <Configure> {%W configure -wraplength [expr {%w - 4}]}
pack $w.msg -side top -fill x

## See Code / Dismiss buttons
set btns [addSeeDismiss $w.buttons $w]
pack $btns -side bottom -fill x

ttk::notebook $w.nb
pack $w.nb -fill both -expand 1 -padx 2m -pady 1m

## The on-screen keyboard: fields of the types of text it knows, which may
## change its layout, buttons to show and hide it, and its state.

set f [ttk::frame $w.nb.keyboard -padding 2m]
$w.nb add $f -text Keyboard -underline 0
foreach {type label} {
    text	Text
    number	Number
    phone	Phone
    email	E-mail
    url		URL
    password	Password
    none	{No keyboard}
} {
    ttk::label $f.l$type -text $label
    ttk::entry $f.e$type
    if {$type eq "password"} {
	$f.e$type configure -show •
    }
    tk keyboard type $f.e$type $type
    grid $f.l$type $f.e$type -sticky ew -pady 1m
}
grid columnconfigure $f 1 -weight 1

ttk::frame $f.b
ttk::button $f.b.show -text "Show" -takefocus 0 \
	-command {tk keyboard show; touchKeyboardState}
ttk::button $f.b.hide -text "Hide" -takefocus 0 \
	-command {tk keyboard hide; touchKeyboardState}
ttk::checkbutton $f.b.manual -text "Manual" -takefocus 0 \
	-variable touchKeyboardManual \
	-command {tk keyboard mode [expr {$touchKeyboardManual ? "manual" : "auto"}]}
pack $f.b.show $f.b.hide $f.b.manual -side left -padx {0 2m}
grid $f.b - -sticky w -pady 1m

ttk::label $f.state
grid $f.state - -sticky w

set touchKeyboardManual [expr {[tk keyboard mode] eq "manual"}]
proc touchKeyboardState {} {
    if {[winfo exists .touch.nb.keyboard.state]} {
	.touch.nb.keyboard.state configure \
		-text "The keyboard is [tk keyboard]."
    }
}
bind $w <<KeyboardShown>> touchKeyboardState
bind $w <<KeyboardHidden>> touchKeyboardState
bind $w <Destroy> {
    if {"%W" eq ".touch"} {
	tk keyboard mode auto
    }
}
touchKeyboardState

## Gestures: a canvas in the mouse touch mode, in which a finger drags the
## square, two fingers pinch it, and a long press posts a menu; and a list
## which scrolls when dragged, and goes on when flung.

set f [ttk::frame $w.nb.gestures -padding 2m]
$w.nb add $f -text Gestures -underline 0
ttk::label $f.msg -anchor w -justify left -text \
	"Drag the square, pinch it to resize it, and press long for a menu.\
	The list below scrolls when it is dragged."
bind $f.msg <Configure> {%W configure -wraplength [expr {%w - 4}]}
canvas $f.c -height 150 -background white -highlightthickness 0
tk touch mode $f.c mouse
$f.c create rectangle 40 30 120 110 -fill SteelBlue -outline {} -tags shape
bind $f.c <ButtonPress-1> {
    set touchDrag [expr {"shape" in [%W gettags current]}]
    set touchLast [list %x %y]
}
bind $f.c <B1-Motion> {
    if {$touchDrag} {
	lassign $touchLast x y
	%W move shape [expr {%x - $x}] [expr {%y - $y}]
	set touchLast [list %x %y]
    }
}
bind $f.c <<Pinch>> {
    %W scale shape %x %y %d %d
}
menu $f.c.menu -tearoff 0
$f.c.menu add command -label "Red" \
	-command [list $f.c itemconfigure shape -fill firebrick]
$f.c.menu add command -label "Blue" \
	-command [list $f.c itemconfigure shape -fill SteelBlue]
$f.c.menu add separator
$f.c.menu add command -label "Reset" \
	-command [list touchResetShape $f.c]
bind $f.c <Button-3> {tk_popup %W.menu %X %Y}
proc touchResetShape {c} {
    $c coords shape 40 30 120 110
}

listbox $f.l -height 6
for {set i 1} {$i <= 100} {incr i} {
    $f.l insert end "Item $i"
}
pack $f.msg -fill x
pack $f.c -fill x -pady 1m
pack $f.l -fill both -expand 1

## Selection: a long press on a word selects it, with handles which move its
## ends, and a toolbar to cut, copy, paste and select all.

set f [ttk::frame $w.nb.selection -padding 2m]
$w.nb add $f -text Selection -underline 0
text $f.t -height 8 -wrap word -font $font
$f.t insert end "Press long on a word of this text to select it. Drag the\
	handles below the selection to move its ends, and use the toolbar\
	above it to cut, copy, paste, or select all. Press long where there is\
	no word to paste at the insertion cursor."
pack $f.t -fill both -expand 1
