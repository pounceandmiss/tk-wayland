# emojichoose.tcl --
#
# Show off the stock emoji picker dialog

if {![info exists widgetDemo]} {
    error "This script should be run from the \"widget\" demo."
}

package require tk

set w .emojichoose
catch {destroy $w}
toplevel $w
wm title $w "Emoji Selection Dialog"
wm iconname $w "emojichooser"
positionWindow $w

label $w.msg -font $font -wraplength 4i -anchor w -justify left -text \
	"This is a demonstration of Tk's support for emojis.\
	To access the emoji picker, use the following platform-specific\
	keyboard shortcuts:\n\n\
	\tmacOS:  Command-Control-Space\n\
	\tWindows:  Windows-.\n\
	\tX11:  Control-.\n\n\The emoji picker will appear near the text or\
	entry widget below and will insert the selected emoji. Without a\
	keyboard, as on a touchscreen, the button below opens it for the\
	widget which last had the focus."
bind $w.msg <Configure> {%W configure -wraplength [expr {%w - 4}]}

pack $w.msg -side top -fill x

## See Code / Dismiss buttons
set btns [addSeeDismiss $w.buttons $w]
pack $btns -side bottom -fill x

## The frame that will contain the widgets.
pack [frame $w.f] -side bottom -expand 1 -fill both -padx 2m -pady 1m

text $w.f.msg -width 40 -height 6
entry $w.f.e -width 40

## The button which opens the picker, for the text or the entry which last
## had the focus. It does not take the focus itself.
set emojiTarget $w.f.msg
foreach widget [list $w.f.msg $w.f.e] {
    bind $widget <FocusIn> {set emojiTarget %W}
}
ttk::button $w.f.choose -text "Insert Emoji…" -takefocus 0 \
	-command {focus $emojiTarget; ::tk::emoji::choose $emojiTarget}
if {![llength [info commands ::tk::emoji::Active]]
	|| ![::tk::emoji::Active]} {
    # The system has its own picker.
    $w.f.choose state disabled
}

pack $w.f.msg $w.f.e -expand 1 -fill both -padx 2m -pady 1m
pack $w.f.choose -anchor w -padx 2m -pady 1m
