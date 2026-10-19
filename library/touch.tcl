# touch.tcl --
#
# Adaptations of Tk to touchscreens, which applications can apply with
# ::tk::touch::Adapt:
#
# - Larger targets: the widgets which take clicks get more padding, and the
#   rows of treeviews and the entries of menus are taller, so that a finger
#   can hit them. Text keeps its size. The ttk settings are applied to the
#   theme in use, and again when the theme changes; those of the classic
#   widgets are defaults of the option database.
# - Selection as on Android: a long press (which tkTouch.c turns into a
#   click of the third button) in an entry, a text, a spinbox or a combobox
#   selects the word there, unless it is in the selection, and shows the
#   selection with two handles, which can be dragged to move its ends, and
#   an edit toolbar above it, to cut, copy, paste and select all. They go
#   away when the selection does, or the focus moves.
#
# Copyright © 2026 The Tk developers.
#
# See the file "license.terms" for information on usage and redistribution
# of this file, and for a DISCLAIMER OF ALL WARRANTIES.

namespace eval ::tk::touch {
    namespace import ::tk::msgcat::mc
}

# ::tk::touch::Adapt --
#
#	Adapt Tk to touchscreens.
#
# Results:
#	None.
#
# Side effects:
#	Sets defaults of the option database and styles of the current ttk
#	theme, and binds the edit menu to the classes of the widgets which
#	edit text.

proc ::tk::touch::Adapt {} {
    variable adapted
    if {[info exists adapted]} {
	return
    }
    set adapted 1

    # Classic widgets.

    foreach {pattern value} {
	*Button.padY		6p
	*Menubutton.padY	6p
	*Checkbutton.padY	4.5p
	*Radiobutton.padY	4.5p
	*Menu.activeBorderWidth	6p
	*Scrollbar.width	13.5p
	*Scale.width		15p
	*Scale.sliderLength	30p
	*Panedwindow.sashWidth	9p
    } {
	option add $pattern $value widgetDefault
    }

    # Ttk widgets, now and when the theme changes.

    Style
    bind all <<ThemeChanged>> {+
	if {"%W" eq "."} {
	    ::tk::touch::Style
	}
    }

    # The selection, with handles and the edit toolbar.

    foreach class {Entry Text Spinbox TEntry TSpinbox TCombobox} {
	bind $class <Button-3> {::tk::touch::LongPress %W %x %y}
    }
}

# ::tk::touch::Style --
#
#	Give the styles of the current ttk theme sizes for fingers.
#
# Results:
#	None.
#
# Side effects:
#	The styles are configured.

proc ::tk::touch::Style {} {
    ttk::style configure TButton -padding {9p 6p}
    ttk::style configure TMenubutton -padding {9p 6p} -arrowsize 6
    ttk::style configure Toolbutton -padding 6p
    foreach style {TCheckbutton TRadiobutton} {
	ttk::style configure $style -padding 4.5p
    }
    foreach style {TEntry TCombobox} {
	ttk::style configure $style -padding 4.5p
    }
    ttk::style configure TCombobox -arrowsize 6
    ttk::style configure TSpinbox -padding {4.5p 4.5p 9p 4.5p} -arrowsize 5
    ttk::style configure TScrollbar -width 13.5p -arrowsize 6
    ttk::style configure TScale -sliderthickness 18p
    ttk::style configure TNotebook.Tab -padding {9p 6p}
    ttk::style configure Heading -padding 4.5p
    ttk::style configure Sash -sashthickness 9p
    ttk::style configure Treeview -rowheight \
	    [expr {[font metrics TkDefaultFont -linespace]
		   + [winfo pixels . 12p]}]
}

# The selection of a widget which edits text, while it is shown with
# handles and an edit toolbar:
#
# Priv(widget)		The widget, or "".
# Priv(top)		Its toplevel, where the handles and the toolbar are.
# Priv(drag)		The handle being dragged (start or end), or "".
# Priv(offset)		Where the handle was grabbed, from its tip.
# Priv(grab)		Where the handle was grabbed, in its window.
# Priv(lineHeight)	The height of the line of the dragged end.
# Priv(cursor)		When there is no selection, the index of the
#			insertion cursor, at which the toolbar is shown.
# Priv(after)		The id of the check of the selection.
# Priv(toolbar)		Whether the toolbar is shown.
# Priv(actions)		The actions in the toolbar.
# Priv(clipboard)	Whether the clipboard had text when the toolbar was
#			shown.

namespace eval ::tk::touch {
    variable Priv
    array set Priv {
	widget {} top {} drag {} offset {0 0} grab {0 0} lineHeight 0
	cursor {} after {} toolbar 0 actions {} clipboard 0
    }
}

# ::tk::touch::LongPress --
#
#	A long press in a widget which edits text: select the word there,
#	unless the point is in the selection, and show the selection with
#	handles and the edit toolbar. Where there is no word, the insertion
#	cursor moves there, and the toolbar is shown at it, to paste.
#
# Arguments:
#	w	The widget.
#	x, y	The point, in the widget.
#
# Results:
#	None.
#
# Side effects:
#	The selection may change; the handles and the toolbar are shown.

proc ::tk::touch::LongPress {w x y} {
    if {[winfo class $w] eq "Text"} {
	set i [$w index @$x,$y]
	set inSelection [expr {[$w tag ranges sel] ne ""
		&& [$w compare $i >= sel.first]
		&& [$w compare $i < sel.last]}]
	if {!$inSelection && ![string is space [$w get $i]]} {
	    SetSelection $w "$i wordstart" "$i wordend"
	}
    } else {
	set i [$w index @$x]
	set inSelection [expr {[$w selection present]
		&& $i >= [$w index sel.first] && $i < [$w index sel.last]}]
	set string [$w get]
	if {!$inSelection && $i < [string length $string]
		&& ![string is space [string index $string $i]]} {
	    set first [tcl_startOfPreviousWord $string [expr {$i + 1}]]
	    if {$first < 0} {
		set first 0
	    }
	    set last [tcl_endOfWord $string $i]
	    if {$last < 0} {
		set last [string length $string]
	    }
	    SetSelection $w $first $last
	}
    }
    if {[Selection $w] eq ""} {
	if {[winfo class $w] eq "Text"} {
	    $w mark set insert @$x,$y
	} else {
	    $w icursor @$x
	}
    }
    focus $w
    ShowSelection $w
}

# ::tk::touch::Selection, ::tk::touch::SetSelection --
#
#	Get or set the selection of a widget which edits text, whether it is
#	a text or an entry.
#
# Results:
#	Selection returns the indices of the first and last characters of
#	the selection, or an empty list.
#
# Side effects:
#	SetSelection sets the selection and moves the insertion cursor to
#	its end.

proc ::tk::touch::Selection {w} {
    if {[winfo class $w] eq "Text"} {
	if {[$w tag ranges sel] eq ""} {
	    return {}
	}
	return [list [$w index sel.first] [$w index sel.last]]
    }
    if {![$w selection present]} {
	return {}
    }
    return [list [$w index sel.first] [$w index sel.last]]
}

proc ::tk::touch::SetSelection {w first last} {
    if {[winfo class $w] eq "Text"} {
	$w tag remove sel 1.0 end
	$w tag add sel $first $last
	$w mark set insert $last
    } else {
	$w selection range $first $last
	$w icursor $last
    }
}

# ::tk::touch::Editable --
#
#	Whether a widget which shows text can be edited.
#
# Results:
#	A boolean.
#
# Side effects:
#	None.

proc ::tk::touch::Editable {w} {
    if {[string match T* [winfo class $w]] && [winfo class $w] ne "Text"} {
	return [$w instate {!disabled !readonly}]
    }
    return [expr {[$w cget -state] eq "normal"}]
}

# ::tk::touch::Ends --
#
#	Where the ends of the selection of a widget are: the bottom left of
#	its first character, and the bottom right of its last one, in root
#	coordinates, with the height of their lines.
#
# Results:
#	A list {x y height} for each end, or an empty list for an end which
#	is not visible.

proc ::tk::touch::Ends {w} {
    lassign [Selection $w] first last
    if {$first eq ""} {
	return {{} {}}
    }
    set rx [winfo rootx $w]
    set ry [winfo rooty $w]
    set isText [expr {[winfo class $w] eq "Text"}]
    if {$isText} {
	set lastChar [$w index "$last - 1 indices"]
    } else {
	set lastChar [expr {$last - 1}]
    }
    set result {}
    foreach index [list $first $lastChar] which {start end} {
	set bbox [$w bbox $index]
	if {[llength $bbox] != 4} {
	    lappend result {}
	    continue
	}
	lassign $bbox x y width height
	if {$which eq "end"
		&& !($isText && [$w get $index] eq "\n")} {
	    incr x $width
	}
	if {$x < 0 || $x > [winfo width $w]
		|| $y + $height < 0 || $y > [winfo height $w]} {
	    lappend result {}
	    continue
	}
	lappend result [list [expr {$rx + $x}] [expr {$ry + $y + $height}] \
		$height]
    }
    return $result
}

# ::tk::touch::CursorPoint --
#
#	Where the insertion cursor of a widget is.
#
# Results:
#	A list {x y height}: its bottom, in root coordinates, and the height
#	of its line; or an empty list if it is not visible.

proc ::tk::touch::CursorPoint {w} {
    set bbox [$w bbox insert]
    if {[llength $bbox] != 4} {
	return {}
    }
    lassign $bbox x y width height
    list [expr {[winfo rootx $w] + $x}] \
	    [expr {[winfo rooty $w] + $y + $height}] $height
}

# ::tk::touch::Child --
#
#	The path of a window of the selection in the toplevel of the widget
#	with the selection: the handles and the toolbar are frames placed in
#	it, above its other children.
#
# Arguments:
#	name	The name of the window.
#
# Results:
#	The path.

proc ::tk::touch::Child {name} {
    variable Priv
    if {$Priv(top) eq "."} {
	return .$name
    }
    return $Priv(top).$name
}

# ::tk::touch::Handle --
#
#	A handle of the selection, created the first time: a square below an
#	end of the selection, which can be dragged.
#
# Arguments:
#	which	start or end.
#
# Results:
#	The window.

proc ::tk::touch::Handle {which} {
    set h [Child __tk_selection_$which]
    if {![winfo exists $h]} {
	set size [winfo pixels . 16p]
	frame $h -width $size -height $size -borderwidth 0 \
		-highlightthickness 0
	tk touch mode $h mouse
	bind $h <ButtonPress-1> [list ::tk::touch::HandlePress $which %X %Y]
	bind $h <B1-Motion> [list ::tk::touch::HandleMove $which %X %Y]
	bind $h <ButtonRelease-1> [list ::tk::touch::HandleRelease]
    }
    return $h
}

# ::tk::touch::Toolbar --
#
#	The edit toolbar, created the first time: buttons to cut, copy,
#	paste and select all, which do not take the focus.
#
# Results:
#	The window.

proc ::tk::touch::Toolbar {} {
    set t [Child __tk_editToolbar]
    if {![winfo exists $t]} {
	frame $t -borderwidth 1 -relief solid
	foreach {action label} {
	    cut Cut copy Copy paste Paste selectall {Select All}
	} {
	    ttk::button $t.$action -text [mc $label] -style Toolbutton \
		    -takefocus 0 -command [list ::tk::touch::Action $action]
	}
    }
    return $t
}

# ::tk::touch::ShowSelection --
#
#	Show the selection of a widget with handles and the edit toolbar,
#	and keep them there while the widget has the selection and the
#	focus.
#
# Results:
#	None.
#
# Side effects:
#	Windows are shown; the selection is checked regularly.

proc ::tk::touch::ShowSelection {w} {
    variable Priv
    if {$Priv(widget) ne ""} {
	HideSelection
    }
    set Priv(widget) $w
    set Priv(top) [winfo toplevel $w]
    set Priv(drag) {}
    set Priv(cursor) [expr {[Selection $w] eq "" ? [$w index insert] : ""}]
    set color [ttk::style lookup . -selectbackground]
    foreach which {start end} {
	[Handle $which] configure -background $color
    }
    Place 1
    after cancel $Priv(after)
    set Priv(after) [after 100 ::tk::touch::Watch]
}

# ::tk::touch::HideSelection --
#
#	Hide the handles and the toolbar.
#
# Results:
#	None.
#
# Side effects:
#	The windows are unplaced.

proc ::tk::touch::HideSelection {} {
    variable Priv
    after cancel $Priv(after)
    if {$Priv(top) ne "" && [winfo exists $Priv(top)]} {
	foreach name {__tk_selection_start __tk_selection_end
		__tk_editToolbar} {
	    set child [Child $name]
	    if {[winfo exists $child]} {
		place forget $child
	    }
	}
    }
    set Priv(widget) {}
    set Priv(drag) {}
    set Priv(toolbar) 0
}

# ::tk::touch::Watch --
#
#	Hide the handles and the toolbar when the widget no longer has the
#	selection or the focus, and follow the selection otherwise (it moves
#	when the widget scrolls).
#
# Results:
#	None.
#
# Side effects:
#	Windows are moved or hidden; schedules itself again.

proc ::tk::touch::Watch {} {
    variable Priv
    set w $Priv(widget)
    if {$w eq "" || ![winfo exists $w] || ![winfo ismapped $w]
	    || [focus -displayof $w] ne $w} {
	HideSelection
	return
    }
    if {[Selection $w] ne ""} {
	set Priv(cursor) {}
    } elseif {$Priv(cursor) eq "" || [$w index insert] ne $Priv(cursor)} {
	# The selection went away, or the insertion cursor moved.

	HideSelection
	return
    }
    if {$Priv(drag) eq ""} {
	Place 0
    }
    set Priv(after) [after 100 ::tk::touch::Watch]
}

# ::tk::touch::Place, ::tk::touch::PlaceHandles, ::tk::touch::MoveWindow --
#
#	Put the handles at the ends of the selection (except the one being
#	dragged, which follows the finger), and the toolbar above it (or
#	below, if there is no room). MoveWindow moves a window, and shows it.
#
# Arguments:
#	toolbar	Whether to show the toolbar, if it is not shown.
#
# Results:
#	None.
#
# Side effects:
#	Windows are moved, shown or hidden.

proc ::tk::touch::PlaceHandles {} {
    variable Priv
    set w $Priv(widget)
    set size [winfo pixels . 16p]
    lassign [Ends $w] start end
    foreach which {start end} {
	if {$which eq $Priv(drag)} {
	    continue
	}
	set h [Handle $which]
	set point [set $which]
	if {$point eq ""} {
	    place forget $h
	    continue
	}
	lassign $point x y
	if {$which eq "start"} {
	    set x [expr {$x - $size}]
	}
	MoveWindow $h $x $y
    }
    return [list $start $end]
}

proc ::tk::touch::MoveWindow {h x y} {
    variable Priv
    set x [expr {round($x) - [winfo rootx $Priv(top)]}]
    set y [expr {round($y) - [winfo rooty $Priv(top)]}]

    # Nothing is done if the window is there already, so that it is not
    # drawn again.

    set info [place info $h]
    if {[winfo manager $h] eq "place" && [dict get $info -x] == $x
	    && [dict get $info -y] == $y} {
	return
    }
    place $h -in $Priv(top) -anchor nw -x $x -y $y
    if {![dict exists $info -x]} {
	raise $h
    }
}

proc ::tk::touch::Place {{toolbar 0}} {
    variable Priv
    set w $Priv(widget)
    set size [winfo pixels . 16p]
    lassign [PlaceHandles] start end

    set t [Toolbar]
    if {!$toolbar && !$Priv(toolbar)} {
	return
    }
    set selected [expr {[Selection $w] ne ""}]
    if {!$selected} {
	set start [CursorPoint $w]
    }
    if {$start eq "" && $end eq ""} {
	place forget $t
	set Priv(toolbar) 0
	return
    }

    # The actions which apply.

    set editable [Editable $w]
    if {[winfo class $w] eq "Text"} {
	set empty [expr {[$w compare 1.0 == "end - 1 indices"]}]
    } else {
	set empty [expr {[$w get] eq ""}]
    }
    if {$toolbar} {
	# The clipboard is only checked when the toolbar is shown.

	set Priv(clipboard) 0
	if {![catch {
	    selection get -displayof $w -selection CLIPBOARD TARGETS
	} targets]} {
	    foreach target {UTF8_STRING STRING TEXT} {
		if {$target in $targets} {
		    set Priv(clipboard) 1
		}
	    }
	}
    }
    set actions {}
    foreach action {cut copy paste selectall} show [list \
	    [expr {$editable && $selected}] $selected \
	    [expr {$editable && $Priv(clipboard)}] [expr {!$empty}]] {
	if {$show} {
	    lappend actions $action
	}
    }
    if {$actions eq ""} {
	place forget $t
	set Priv(toolbar) 0
	return
    }
    if {$actions ne $Priv(actions) || [pack slaves $t] eq ""} {
	foreach child [winfo children $t] {
	    pack forget $child
	}
	foreach action $actions {
	    pack $t.$action -side left
	}
	set Priv(actions) $actions
	update idletasks
    }

    # Centered above the selection, in the toplevel.

    set ends [lmap point [list $start $end] {
	if {$point eq ""} continue
	set point
    }]
    set top [lindex [lsort -integer [lmap point $ends {
	expr {[lindex $point 1] - [lindex $point 2]}
    }]] 0]
    set bottom [lindex [lsort -integer [lmap point $ends {
	lindex $point 1
    }]] end]
    set xs [lmap point $ends {lindex $point 0}]
    set middle [expr {([tcl::mathfunc::min {*}$xs]
	    + [tcl::mathfunc::max {*}$xs]) / 2}]
    set width [winfo reqwidth $t]
    set height [winfo reqheight $t]
    set left [winfo rootx $Priv(top)]
    set x [expr {max($left, min($middle - $width / 2,
	    $left + [winfo width $Priv(top)] - $width))}]
    set y [expr {$top - $height - [winfo pixels . 6p]}]
    if {$y < [winfo rooty $Priv(top)]} {
	set y [expr {$bottom + $size + [winfo pixels . 6p]}]
    }
    MoveWindow $t $x $y
    set Priv(toolbar) 1
}

# ::tk::touch::HandlePress, ::tk::touch::HandleMove,
# ::tk::touch::HandleRelease --
#
#	Drag a handle: the end of the selection follows the point of the
#	handle. The toolbar is hidden while dragging.
#
# Arguments:
#	which	The handle, start or end.
#	X, Y	The pointer, in root coordinates.
#
# Results:
#	None.
#
# Side effects:
#	The selection changes.

proc ::tk::touch::HandlePress {which X Y} {
    variable Priv
    set w $Priv(widget)
    if {$w eq ""} {
	return
    }
    set point [lindex [Ends $w] [expr {$which eq "end"}]]
    if {$point eq ""} {
	return
    }
    lassign $point x y height
    set h [Handle $which]
    set Priv(drag) $which
    set Priv(offset) [list [expr {$X - $x}] [expr {$Y - $y}]]
    set Priv(grab) [list [expr {$X - [winfo rootx $h]}] \
	    [expr {$Y - [winfo rooty $h]}]]
    set Priv(lineHeight) $height
    place forget [Toolbar]
    set Priv(toolbar) 0
}

proc ::tk::touch::HandleMove {which X Y} {
    variable Priv
    set w $Priv(widget)
    if {$w eq "" || $Priv(drag) eq "" || ![winfo exists $w]} {
	return
    }
    # The handle follows the finger.

    lassign $Priv(grab) gx gy
    MoveWindow [Handle $Priv(drag)] [expr {$X - $gx}] [expr {$Y - $gy}]

    lassign $Priv(offset) dx dy
    set x [expr {$X - $dx - [winfo rootx $w]}]
    set y [expr {$Y - $dy - $Priv(lineHeight) / 2 - [winfo rooty $w]}]
    lassign [Selection $w] first last
    if {$first eq ""} {
	return
    }
    if {[winfo class $w] eq "Text"} {
	set index [$w index @[expr {round($x)}],[expr {round($y)}]]
	$w see $index
	set other [expr {$Priv(drag) eq "start" ? $last : $first}]
	if {[$w compare $index == $other]} {
	    return
	}
	if {[$w compare $index > $other]} {
	    SetSelection $w $other $index
	    SwapDrag end
	} else {
	    SetSelection $w $index $other
	    SwapDrag start
	}
    } else {
	if {$x < 0} {
	    $w xview scroll -1 units
	} elseif {$x > [winfo width $w]} {
	    $w xview scroll 1 units
	}
	set index [$w index @[expr {round($x)}]]
	set other [expr {$Priv(drag) eq "start" ? $last : $first}]
	if {$index == $other} {
	    return
	}
	if {$index > $other} {
	    SetSelection $w $other $index
	    SwapDrag end
	} else {
	    SetSelection $w $index $other
	    SwapDrag start
	}
    }
    PlaceHandles
}

# ::tk::touch::SwapDrag --
#
#	The end of the selection being dragged crossed the other end: the
#	dragged end becomes the other one, and the finger takes the handle
#	of that end, which goes where the dragged one was.
#
# Arguments:
#	which	The end being dragged now.
#
# Results:
#	None.
#
# Side effects:
#	The handles move.

proc ::tk::touch::SwapDrag {which} {
    variable Priv
    if {$which eq $Priv(drag)} {
	return
    }
    set from [Handle $Priv(drag)]
    set to [Handle $which]
    MoveWindow $to [winfo rootx $from] [winfo rooty $from]
    set Priv(drag) $which
}

proc ::tk::touch::HandleRelease {} {
    variable Priv
    if {$Priv(drag) eq ""} {
	return
    }
    set Priv(drag) {}
    if {$Priv(widget) ne ""} {
	Place 1
    }
}

# ::tk::touch::Action --
#
#	Do an action of the edit toolbar to the widget with the selection.
#
# Arguments:
#	action	cut, copy, paste or selectall.
#
# Results:
#	None.
#
# Side effects:
#	The text, the clipboard or the selection change. The handles and
#	the toolbar are hidden after cutting, copying and pasting.

proc ::tk::touch::Action {action} {
    variable Priv
    set w $Priv(widget)
    if {$w eq "" || ![winfo exists $w]} {
	return
    }
    switch -- $action {
	cut {event generate $w <<Cut>>}
	copy {event generate $w <<Copy>>}
	paste {event generate $w <<Paste>>}
	selectall {
	    event generate $w <<SelectAll>>
	    Place 1
	    return
	}
    }
    HideSelection
    if {$action eq "copy"} {
	if {[winfo class $w] eq "Text"} {
	    $w tag remove sel 1.0 end
	} else {
	    $w selection clear
	}
    }
}

# Local Variables:
# mode: tcl
# fill-column: 78
# End:
