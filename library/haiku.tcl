# haiku.tcl --
#
# Scripting of the widgets of Tk on Haiku: the requests of the scripting
# protocol of Haiku (as hey sends them) for the property "Widget" of the
# application or of a window, which haiku/tkHaikuScript.c gives to
# ::tk::haiku::Script. The properties are those of the controls and views
# of Haiku:
#
#	count Widget			The number of widgets.
#	get Path of Widget ...		The path name.
#	get Class of Widget ...		The class.
#	get Label of Widget ...		The text a button or label shows.
#	get|set Text of Widget ...	The text of an entry or text widget.
#	get|set Value of Widget ...	The value of a check or radio button
#					(1 or 0), a scale, a spinbox or a
#					progress bar.
#	get|set Enabled of Widget ...	Whether the widget is not disabled.
#	get|set Focus of Widget ...	Whether it has the focus.
#	get Frame of Widget ...		Its rectangle on the screen.
#	do Widget ...			Invoke a button, check or radio
#					button (or do the accessible action).
#
# A widget is named by its path name, or by its label (or accessible name),
# or has an index among the widgets of its window (or of all the windows),
# in stacking order, parents first.
#
# Copyright © 2026 The Tk developers.
#
# See the file "license.terms" for information on usage and redistribution
# of this file, and for a DISCLAIMER OF ALL WARRANTIES.

namespace eval ::tk::haiku {
    namespace export Script
}

# ::tk::haiku::Script --
#
#	Handle a scripting request.
#
# Arguments:
#	verb	get, set, do or count.
#	property The property asked (Path, Text, ...), or "" for do and count.
#	scope	The toplevel whose window the request is for, or "" for the
#		application.
#	form	How the widget is given: name, index, reverse (an index from
#		the end, 1 for the last widget), or "" for count.
#	widget	The name or index of the widget.
#	value	The new value, for set.
#
# Results:
#	A list of the type of the result (bool, int32, float, rect, string or
#	none) and its value; an error if the request cannot be done.

proc ::tk::haiku::Script {verb property scope form widget {value {}}} {
    if {$verb eq "count"} {
	return [list int32 [llength [Widgets $scope]]]
    }
    set w [Find $scope $form $widget]
    if {$verb eq "do"} {
	if {$property ne ""} {
	    return -code error "\"do\" is for widgets, not for $property"
	}
	return [Invoke $w]
    }
    if {$verb ni {get set}} {
	return -code error "unknown verb \"$verb\""
    }
    switch -- $property {
	Path - Class - Label - Frame {
	    if {$verb eq "set"} {
		return -code error "$property cannot be set"
	    }
	    return [$property $w]
	}
	Text - Value - Enabled - Focus {
	    if {$verb eq "set"} {
		Set$property $w $value
		return [list none {}]
	    }
	    return [$property $w]
	}
	default {
	    return -code error "unknown property \"$property\""
	}
    }
}

# ::tk::haiku::Widgets --
#
#	The widgets of a toplevel (without the toplevels in it), or of all
#	the toplevels, in stacking order, parents first.

proc ::tk::haiku::Widgets {scope} {
    if {$scope eq ""} {
	set result {}
	foreach top [Toplevels .] {
	    lappend result {*}[Widgets $top]
	}
	return $result
    }
    set result [list $scope]
    foreach child [winfo children $scope] {
	if {[winfo toplevel $child] eq $child} {
	    continue
	}
	lappend result {*}[Widgets $child]
    }
    return $result
}

proc ::tk::haiku::Toplevels {w} {
    set result {}
    if {[winfo toplevel $w] eq $w} {
	lappend result $w
    }
    foreach child [winfo children $w] {
	lappend result {*}[Toplevels $child]
    }
    return $result
}

# ::tk::haiku::Find --
#
#	The widget a specifier names.

proc ::tk::haiku::Find {scope form widget} {
    set widgets [Widgets $scope]
    switch -- $form {
	index - reverse {
	    # A reverse index counts from the end, as with the views of
	    # Haiku: hey gives -1, the last widget, as a reverse index 1.
	    if {$form eq "reverse" && [string is integer -strict $widget]} {
		set widget [expr {[llength $widgets] - $widget}]
	    }
	    if {![string is integer -strict $widget]
		    || $widget < 0 || $widget >= [llength $widgets]} {
		return -code error "no widget $widget"
	    }
	    return [lindex $widgets $widget]
	}
	name {
	    if {[string match .* $widget]} {
		if {$widget in $widgets} {
		    return $widget
		}
	    } else {
		foreach w $widgets {
		    if {[Name $w] eq $widget} {
			return $w
		    }
		}
	    }
	    return -code error "no widget \"$widget\""
	}
	default {
	    return -code error "a widget is named or has an index"
	}
    }
}

# ::tk::haiku::Name --
#
#	The name of a widget for scripting: its accessible name, or the text
#	it shows, or the text of a label just before it.

proc ::tk::haiku::Name {w} {
    if {![catch {::tk::accessible::get_acc_name $w} name] && $name ne ""} {
	return $name
    }
    return [lindex [Label $w] 1]
}

# Getters, which return the type of the value and the value.

proc ::tk::haiku::Path {w} {
    list string $w
}

proc ::tk::haiku::Class {w} {
    list string [winfo class $w]
}

proc ::tk::haiku::Label {w} {
    foreach option {-text -label} {
	if {![catch {$w cget $option} text]} {
	    return [list string $text]
	}
    }
    list string {}
}

proc ::tk::haiku::Frame {w} {
    # The rectangle of the pixels, in pixels of the screen.
    set x [winfo rootx $w]
    set y [winfo rooty $w]
    list rect [list $x $y [expr {$x + [winfo width $w] - 1}] \
	    [expr {$y + [winfo height $w] - 1}]]
}

proc ::tk::haiku::Text {w} {
    switch -- [winfo class $w] {
	Entry - Spinbox - TEntry - TSpinbox - TCombobox {
	    return [list string [$w get]]
	}
	Text {
	    return [list string [$w get 1.0 end-1c]]
	}
    }
    return -code error "[winfo class $w] has no Text"
}

proc ::tk::haiku::Value {w} {
    switch -- [winfo class $w] {
	Checkbutton - TCheckbutton - Radiobutton - TRadiobutton {
	    upvar #0 [$w cget -variable] var
	    set on [expr {[string match *Check* [winfo class $w]]
		    ? [$w cget -onvalue] : [$w cget -value]}]
	    return [list int32 [expr {[info exists var] && $var eq $on}]]
	}
	Scale - TScale - TProgressbar {
	    return [list float [expr {double([$w get])}]]
	}
	Spinbox - TSpinbox {
	    if {[string is double -strict [$w get]]} {
		return [list float [expr {double([$w get])}]]
	    }
	    return [list string [$w get]]
	}
    }
    return -code error "[winfo class $w] has no Value"
}

proc ::tk::haiku::Enabled {w} {
    if {![catch {$w instate disabled} disabled]} {
	return [list bool [expr {!$disabled}]]
    }
    if {![catch {$w cget -state} state]} {
	return [list bool [expr {$state ne "disabled"}]]
    }
    list bool 1
}

proc ::tk::haiku::Focus {w} {
    list bool [expr {[focus] eq $w}]
}

# Setters.

proc ::tk::haiku::SetText {w value} {
    switch -- [winfo class $w] {
	Entry - Spinbox - TEntry - TSpinbox - TCombobox {
	    $w delete 0 end
	    $w insert 0 $value
	    return
	}
	Text {
	    $w delete 1.0 end
	    $w insert 1.0 $value
	    return
	}
    }
    return -code error "[winfo class $w] has no Text"
}

proc ::tk::haiku::SetValue {w value} {
    switch -- [winfo class $w] {
	Checkbutton - TCheckbutton {
	    # Ttk buttons have no select and deselect: set the variable.
	    upvar #0 [$w cget -variable] var
	    set var [$w cget [expr {[string is true -strict $value]
		    ? "-onvalue" : "-offvalue"}]]
	    return
	}
	Radiobutton - TRadiobutton {
	    if {[string is true -strict $value]} {
		upvar #0 [$w cget -variable] var
		set var [$w cget -value]
	    }
	    return
	}
	Scale - TScale - Spinbox - TSpinbox {
	    $w set $value
	    return
	}
	TProgressbar {
	    $w configure -value $value
	    return
	}
    }
    return -code error "[winfo class $w] has no Value"
}

proc ::tk::haiku::SetEnabled {w value} {
    set enabled [string is true -strict $value]
    if {![catch {$w instate disabled}]} {
	$w state [expr {$enabled ? "!disabled" : "disabled"}]
    } elseif {[catch {$w configure -state \
	    [expr {$enabled ? "normal" : "disabled"}]}]} {
	return -code error "[winfo class $w] cannot be disabled"
    }
}

proc ::tk::haiku::SetFocus {w value} {
    if {[string is true -strict $value]} {
	focus -force $w
    } elseif {[focus] eq $w} {
	focus [winfo toplevel $w]
    }
}

# ::tk::haiku::Invoke --
#
#	Do what a click on a button does.

proc ::tk::haiku::Invoke {w} {
    switch -- [winfo class $w] {
	Button - TButton - Checkbutton - TCheckbutton - Radiobutton
	- TRadiobutton {
	    if {![lindex [Enabled $w] 1]} {
		return -code error "$w is disabled"
	    }
	    uplevel #0 [list $w invoke]
	    return [list none {}]
	}
    }
    if {![catch {::tk::accessible::get_acc_action $w} action]
	    && $action ne ""} {
	uplevel #0 $action
	return [list none {}]
    }
    return -code error "[winfo class $w] cannot be invoked"
}
