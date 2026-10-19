# headerbar.tcl --
#
#	A window with a title bar written in Tcl, with the ::tk::wayland
#	commands: the compositor does not decorate the window, which moves and
#	resizes itself when the user drags its header bar and its edges, and
#	has rounded corners.
#
#	wish headerbar.tcl
#
# Copyright © 2026 The Tk developers.
#
# See the file "license.terms" for information on usage and redistribution of
# this file, and for a DISCLAIMER OF ALL WARRANTIES.

package require Tk 9

if {[tk windowingsystem] ne "wayland"} {
    error "this demo needs Tk on Wayland, not [tk windowingsystem]"
}

namespace eval headerbar {
    variable border 6		;# Width of the edges which resize the window.
    variable corner 18		;# Length of the corners along the edges.
    variable cursors		;# Cursors of the widgets under the edges.
    variable press {}		;# Where button 1 was pressed in the header.
    variable inhibit 0
    variable rounded 1		;# Whether the window has rounded corners.
    variable radius 12		;# Their radius.
}

# The look of the header bar, after the theme.

proc headerbar::style {} {
    set bg [ttk::style lookup TFrame -background]
    lassign [winfo rgb . $bg] r g b
    set dark [expr {$r + $g + $b < 3 * 0x8000}]
    set header [expr {$dark ? "#303030" : "#e8e8e7"}]
    set inactive [expr {$dark ? "#242424" : "#fafafa"}]
    set edge [expr {$dark ? "#1b1b1b" : "#b6b6b3"}]
    ttk::style configure Header.TFrame -background $header
    ttk::style configure Header.TLabel -background $header
    ttk::style configure Inactive.Header.TFrame -background $inactive
    ttk::style configure Inactive.Header.TLabel -background $inactive \
	    -foreground [expr {$dark ? "#909090" : "#8a8a8a"}]
    ttk::style configure Title.Header.TLabel -font TkHeadingFont
    ttk::style configure Title.Inactive.Header.TLabel -font TkHeadingFont
    ttk::style configure Header.Toolbutton -background $header -padding 4
    ttk::style configure Inactive.Header.Toolbutton -background $inactive
    . configure -background $edge
}

proc headerbar::build {} {
    wm title . "Header bar"
    wm minsize . 380 260
    wm geometry . 560x400
    ::tk::wayland::appid . org.tcl-lang.tk.HeaderbarDemo
    ::tk::wayland::decorations . none
    roundCorners

    # The header: a menu, the title and the buttons of the window.

    set h [ttk::frame .header -style Header.TFrame -padding {6 4}]
    ttk::menubutton $h.menu -text ☰ -style Header.Toolbutton \
	    -menu $h.menu.m
    menu $h.menu.m -tearoff 0
    $h.menu.m add checkbutton -label "Keep the screen on" \
	    -variable [namespace current]::inhibit \
	    -command {::tk::wayland::inhibitidle . $::headerbar::inhibit}
    $h.menu.m add checkbutton -label "Rounded corners" \
	    -variable [namespace current]::rounded \
	    -command [namespace code roundCorners]
    $h.menu.m add command -label "Use the compositor's title bar" \
	    -command [namespace code {decorate server}]
    $h.menu.m add command -label "Window menu" \
	    -command {::tk::wayland::windowmenu . 0 0}
    $h.menu.m add separator
    $h.menu.m add command -label Quit -command exit
    ttk::label $h.title -style Title.Header.TLabel -anchor center \
	    -textvariable [namespace current]::title
    ttk::label $h.subtitle -style Header.TLabel -anchor center \
	    -textvariable [namespace current]::subtitle
    ttk::button $h.min -text – -style Header.Toolbutton -width 2 \
	    -command {wm iconify .}
    ttk::button $h.max -text □ -style Header.Toolbutton -width 2 \
	    -command [namespace code toggleMaximized]
    ttk::button $h.close -text ✕ -style Header.Toolbutton -width 2 \
	    -command [namespace code close]
    grid $h.menu $h.title $h.min $h.max $h.close -sticky ns
    grid ^ $h.subtitle ^ ^ ^ -sticky ns
    grid configure $h.title $h.subtitle -sticky ew
    grid columnconfigure $h 1 -weight 1

    foreach w [list $h $h.title $h.subtitle] {
	bind $w <ButtonPress-1> [namespace code {headerPress %X %Y}]
	bind $w <B1-Motion> [namespace code {headerMotion %X %Y}]
	bind $w <ButtonRelease-1> [namespace code {set press {}}]
	bind $w <Double-ButtonPress-1> [namespace code toggleMaximized]
	bind $w <ButtonPress-3> {::tk::wayland::windowmenu .}
    }

    # The content.

    set c [ttk::frame .content -padding 12]
    text $c.text -wrap word -height 8 -relief flat -highlightthickness 0 \
	    -font TkTextFont -padx 6 -pady 6
    $c.text insert end [string trim {
The title bar of this window is made of Tk widgets.

Drag the header bar to move the window, double-click it to maximize the
window, and press the right button on it for the menu of the compositor.
Drag the edges of the window to resize it.

The window is undecorated with ::tk::wayland::decorations, moves and resizes
with ::tk::wayland::startmove and ::tk::wayland::startresize, and shows the
window menu with ::tk::wayland::windowmenu. Its corners are rounded with
::tk::wayland::corners.
    }]
    $c.text configure -state disabled
    ttk::checkbutton $c.rounded -text "Rounded corners" \
	    -variable [namespace current]::rounded \
	    -command [namespace code roundCorners]
    ttk::button $c.decorate -text "Use the compositor's title bar" \
	    -command [namespace code toggleDecorations]
    grid $c.text - -sticky nsew
    grid $c.rounded $c.decorate -pady {12 0}
    grid configure $c.rounded -sticky w
    grid configure $c.decorate -sticky e
    grid columnconfigure $c 1 -weight 1
    grid rowconfigure $c 0 -weight 1

    # The edges of the window resize it: the widgets along them have a tag
    # whose bindings come before theirs. The toplevel shows as an outline.

    bind HeaderbarEdge <Enter> [namespace code {edgeCursor %W %X %Y}]
    bind HeaderbarEdge <Motion> [namespace code {edgeCursor %W %X %Y}]
    bind HeaderbarEdge <Leave> [namespace code {edgeCursor %W {} {}}]
    bind HeaderbarEdge <ButtonPress-1> [namespace code {edgePress %X %Y}]
    edgeTags .
    bind . <Configure> [namespace code {if {"%W" eq "."} refresh}]
    bind . <FocusIn> [list after idle [namespace code refresh]]
    bind . <FocusOut> [list after idle [namespace code refresh]]
    bind . <<ThemeChanged>> [namespace code style]

    style
    decorate none
}

proc headerbar::decorate {mode} {
    ::tk::wayland::decorations . $mode
    if {$mode eq "none"} {
	grid .header -row 0 -sticky ew
	.content.rounded state !disabled
	.content.decorate configure -text "Use the compositor's title bar"
    } else {
	grid forget .header
	.content.rounded state disabled
	.content.decorate configure -text "Use the title bar of Tk"
    }
    grid .content -row 1 -sticky nsew
    grid columnconfigure . 0 -weight 1
    grid rowconfigure . 1 -weight 1
    update
}

# The corners are rounded only without the compositor's title bar.

proc headerbar::roundCorners {} {
    variable rounded
    variable radius
    ::tk::wayland::corners . [expr {$rounded ? $radius : 0}]
}

proc headerbar::toggleDecorations {} {
    if {[::tk::wayland::decorations .] eq "none"} {
	decorate server
    } else {
	decorate none
    }
}

# Closing the window, as the compositor would: with the WM_DELETE_WINDOW
# protocol.

proc headerbar::close {} {
    set command [wm protocol . WM_DELETE_WINDOW]
    if {$command ne ""} {
	uplevel #0 $command
    } else {
	destroy .
    }
}

proc headerbar::toggleMaximized {} {
    wm attributes . -zoomed [expr {![wm attributes . -zoomed]}]
}

# Update the window after its state: no outline when maximized or with a
# compositor's title bar, the header greyed out when inactive.

proc headerbar::refresh {} {
    variable title
    variable subtitle

    set undecorated [expr {[::tk::wayland::decorations .] eq "none"}]
    set maximized [wm attributes . -zoomed]
    set full [wm attributes . -fullscreen]
    set pad [expr {$undecorated && !$maximized && !$full ? 1 : 0}]
    . configure -padx $pad -pady $pad
    .header.max configure -text [expr {$maximized ? "❐" : "□"}]

    set active [expr {[focus] ne "" && [winfo toplevel [focus]] eq "."}]
    set prefix [expr {$active ? "" : "Inactive."}]
    .header configure -style ${prefix}Header.TFrame
    .header.title configure -style Title.${prefix}Header.TLabel
    .header.subtitle configure -style ${prefix}Header.TLabel
    foreach b {menu min max close} {
	.header.$b configure -style ${prefix}Header.Toolbutton
    }

    set title [wm title .]
    set subtitle [format "%s · scale %g" \
	    [::tk::wayland::appid .] [::tk::wayland::scale .]]
}

# The header bar starts moving the window when the pointer moves with the
# button pressed, so that double-clicking it does not move the window.

proc headerbar::headerPress {x y} {
    variable press [list $x $y]
}

proc headerbar::headerMotion {x y} {
    variable press
    if {$press eq ""} {
	return
    }
    lassign $press x0 y0
    if {abs($x - $x0) > 3 || abs($y - $y0) > 3} {
	set press {}
	::tk::wayland::startmove .
    }
}

# Give the tag of the edges to a window and its descendants.

proc headerbar::edgeTags {w} {
    if {"HeaderbarEdge" ni [bindtags $w]} {
	bindtags $w [linsert [bindtags $w] 0 HeaderbarEdge]
    }
    foreach child [winfo children $w] {
	if {[winfo toplevel $child] eq "."} {
	    edgeTags $child
	}
    }
}

# The edge of the window at a point of the screen, or "" if none, when the
# window can be resized.

proc headerbar::edge {X Y} {
    variable border
    variable corner
    if {$X eq "" || [. cget -padx] == 0} {
	return ""
    }
    set x [expr {$X - [winfo rootx .]}]
    set y [expr {$Y - [winfo rooty .]}]
    set width [winfo width .]
    set height [winfo height .]
    set v [expr {$y < $border ? "n" : $y >= $height - $border ? "s" : ""}]
    set h [expr {$x < $border ? "w" : $x >= $width - $border ? "e" : ""}]

    # Near the corners, the edges resize in both directions.
    if {$v ne "" && $h eq "" && ($x < $corner || $x >= $width - $corner)} {
	set h [expr {$x < $corner ? "w" : "e"}]
    }
    if {$h ne "" && $v eq "" && ($y < $corner || $y >= $height - $corner)} {
	set v [expr {$y < $corner ? "n" : "s"}]
    }
    return $v$h
}

# Show the cursor of the edge under the pointer, and give the widget its own
# cursor back away from the edges.

proc headerbar::edgeCursor {w X Y} {
    variable cursors
    set edge [edge $X $Y]

    # Ancestors of the widget under the pointer get <Enter> too: they keep
    # their cursor, which the widget would inherit.
    if {$edge ne "" && [winfo containing $X $Y] ne $w} {
	set edge ""
    }
    set cursor [dict get {
	"" {} n top_side s bottom_side w left_side e right_side
	nw top_left_corner ne top_right_corner
	sw bottom_left_corner se bottom_right_corner
    } $edge]
    if {[catch {$w cget -cursor} current]} {
	return
    }
    if {$cursor ne ""} {
	if {![info exists cursors($w)]} {
	    set cursors($w) $current
	}
	if {$current ne $cursor} {
	    $w configure -cursor $cursor
	}
    } elseif {[info exists cursors($w)]} {
	$w configure -cursor $cursors($w)
	unset cursors($w)
    }
}

proc headerbar::edgePress {X Y} {
    set edge [edge $X $Y]
    if {$edge ne ""} {
	::tk::wayland::startresize . $edge
	return -code break
    }
}

headerbar::build
