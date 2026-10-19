# multitouch.tcl --
#
# This demonstration script is a canvas for finger painting, in the raw touch
# mode ("tk touch mode $c raw"): every finger which touches it sends it
# <<TouchBegin>>, <<TouchMove>> and <<TouchEnd>> events, with the id of the
# finger as data (%d), so that each finger draws its own stroke. The mouse is
# one more finger.

if {![info exists widgetDemo]} {
    error "This script should be run from the \"widget\" demo."
}

package require tk

set w .multitouch
catch {destroy $w}
toplevel $w
wm title $w "Multitouch Finger Painting"
wm iconname $w "multitouch"
positionWindow $w

label $w.msg -font $font -wraplength 4i -anchor w -justify left -text \
	"Paint with as many fingers as the screen follows: each finger draws\
	its own stroke, the fingers on the screen are joined, and the strokes\
	fade away when the fingers are lifted. The mouse paints too."
bind $w.msg <Configure> {%W configure -wraplength [expr {%w - 4}]}
pack $w.msg -side top -fill x

## See Code / Dismiss buttons
set btns [addSeeDismiss $w.buttons $w]
pack $btns -side bottom -fill x

ttk::frame $w.bar
ttk::button $w.bar.clear -text "Clear" -takefocus 0 \
	-command {.multitouch.c delete stroke}
ttk::checkbutton $w.bar.keep -text "Keep strokes" -takefocus 0 \
	-variable multitouch(keep)
ttk::label $w.bar.count -textvariable multitouch(count)
pack $w.bar.clear $w.bar.keep -side left -padx {0 2m}
pack $w.bar.count -side right
pack $w.bar -side bottom -fill x -padx 2m -pady 1m

set c $w.c
canvas $c -width 300 -height 300 -background #101018 -highlightthickness 0
pack $c -fill both -expand 1 -padx 2m -pady 1m
tk touch mode $c raw

## The state of the fingers: for each finger on the canvas, its color, the
## line of its stroke and its last position; the order in which they
## touched gives the colors.

array set multitouch {keep 0 count {} next 0}
set multitouch(fingers) {}
set multitouch(colors) {
    #ff4f6e #ffb84d #ffe94d #6dff8f #4dd8ff #6b7bff #c66bff #ff6bd1
}
set multitouch(background) #101018

proc multitouchBegin {c id x y} {
    global multitouch
    set colors $multitouch(colors)
    set color [lindex $colors [expr {$multitouch(next) % [llength $colors]}]]
    incr multitouch(next)

    # The stroke, and its glow: a wider line of a darker color below it.

    set glow [$c create line $x $y $x $y -fill [multitouchMix $color 0.6] \
	    -width 18 -capstyle round -joinstyle round -smooth 1 \
	    -tags {stroke glow}]
    set line [$c create line $x $y $x $y -fill $color -width 6 \
	    -capstyle round -joinstyle round -smooth 1 -tags stroke]
    $c raise $line $glow

    # The ring which follows the finger, with its number.

    set ring [$c create oval [expr {$x - 34}] [expr {$y - 34}] \
	    [expr {$x + 34}] [expr {$y + 34}] -outline $color -width 3 \
	    -tags ring]
    set label [$c create text $x [expr {$y - 48}] -fill $color \
	    -text [llength [dict keys $multitouch(fingers)]] \
	    -font {Helvetica 12 bold} -tags ring]
    dict set multitouch(fingers) $id \
	    [dict create color $color line $line glow $glow ring $ring \
		label $label x $x y $y]
    multitouchRipple $c $x $y $color 0
    multitouchJoin $c
}

proc multitouchMove {c id x y} {
    global multitouch
    if {![dict exists $multitouch(fingers) $id]} {
	return
    }
    set finger [dict get $multitouch(fingers) $id]
    if {hypot($x - [dict get $finger x], $y - [dict get $finger y]) < 2} {
	return
    }
    foreach item [list [dict get $finger line] [dict get $finger glow]] {
	$c coords $item {*}[$c coords $item] $x $y
    }
    $c moveto [dict get $finger ring] [expr {$x - 35}] [expr {$y - 35}]
    $c coords [dict get $finger label] $x [expr {$y - 48}]
    dict set multitouch(fingers) $id x $x
    dict set multitouch(fingers) $id y $y
    multitouchJoin $c
}

proc multitouchEnd {c id} {
    global multitouch
    if {![dict exists $multitouch(fingers) $id]} {
	return
    }
    set finger [dict get $multitouch(fingers) $id]
    $c delete [dict get $finger ring] [dict get $finger label]
    dict unset multitouch(fingers) $id
    multitouchRipple $c [dict get $finger x] [dict get $finger y] \
	    [dict get $finger color] 0
    if {!$multitouch(keep)} {
	multitouchFade $c [dict get $finger line] [dict get $finger glow] \
		[dict get $finger color] 0
    }
    multitouchJoin $c
}

## Join the fingers on the canvas with lines, and fill the polygon between
## them when there are three or more.

proc multitouchJoin {c} {
    global multitouch
    $c delete web
    set points {}
    dict for {id finger} $multitouch(fingers) {
	lappend points [list [dict get $finger x] [dict get $finger y]]
    }
    set n [llength $points]
    set multitouch(count) [expr {$n == 1 ? "1 finger" : "$n fingers"}]
    if {$n == 0} {
	set multitouch(count) {}
    }
    if {$n >= 3} {
	# Around the center, so that the polygon does not cross itself.

	set cx 0.0
	set cy 0.0
	foreach p $points {
	    set cx [expr {$cx + [lindex $p 0] / $n}]
	    set cy [expr {$cy + [lindex $p 1] / $n}]
	}
	set points [lsort -real -command {apply {{a b} {
	    expr {[lindex $a 0] < [lindex $b 0] ? -1 : 1}
	}}} [lmap p $points {
	    list [expr {atan2([lindex $p 1] - $cy, [lindex $p 0] - $cx)}] $p
	}]]
	set points [lmap p $points {lindex $p 1}]
	$c create polygon {*}[concat {*}$points] -fill #6b7bff \
		-stipple gray12 -outline {} -tags web
    }
    for {set i 0} {$i < $n} {incr i} {
	for {set j [expr {$i + 1}]} {$j < $n} {incr j} {
	    $c create line {*}[lindex $points $i] {*}[lindex $points $j] \
		    -fill #8890b0 -dash {4 4} -tags web
	}
    }
    $c lower web
}

## A ring which grows and fades where a finger touched or was lifted.

proc multitouchRipple {c x y color step} {
    if {![winfo exists $c]} {
	return
    }
    set item ripple$x,$y,$color
    $c delete $item
    if {$step >= 12} {
	return
    }
    set r [expr {10 + 6 * $step}]
    $c create oval [expr {$x - $r}] [expr {$y - $r}] [expr {$x + $r}] \
	    [expr {$y + $r}] -outline [multitouchMix $color [expr {$step / 12.0}]] \
	    -width [expr {max(1, 6 - $step / 2)}] -tags [list ripple $item]
    after 30 [list multitouchRipple $c $x $y $color [incr step]]
}

## Fade a stroke into the background, then delete it.

proc multitouchFade {c line glow color step} {
    if {![winfo exists $c] || [$c type $line] eq ""} {
	return
    }
    if {$step >= 20} {
	$c delete $line $glow
	return
    }
    set t [expr {$step / 20.0}]
    $c itemconfigure $line -fill [multitouchMix $color $t]
    $c itemconfigure $glow -fill [multitouchMix $color [expr {0.6 + 0.4 * $t}]]
    after 60 [list multitouchFade $c $line $glow $color [incr step]]
}

## Mix a color with the background: t = 0 gives the color, 1 the background.

proc multitouchMix {color t} {
    global multitouch
    scan $color #%2x%2x%2x r g b
    scan $multitouch(background) #%2x%2x%2x br bg bb
    format #%02x%02x%02x [expr {round($r + ($br - $r) * $t)}] \
	    [expr {round($g + ($bg - $g) * $t)}] \
	    [expr {round($b + ($bb - $b) * $t)}]
}

bind $c <<TouchBegin>> {multitouchBegin %W %d %x %y}
bind $c <<TouchMove>> {multitouchMove %W %d %x %y}
bind $c <<TouchEnd>> {multitouchEnd %W %d}
bind $c <<TouchCancel>> {multitouchEnd %W %d}

# The mouse is one more finger.

bind $c <ButtonPress-1> {multitouchBegin %W mouse %x %y}
bind $c <B1-Motion> {multitouchMove %W mouse %x %y}
bind $c <ButtonRelease-1> {multitouchEnd %W mouse}
bind $c <Destroy> {array set multitouch {fingers {} next 0 count {}}}
