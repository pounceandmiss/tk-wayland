# An analog clock, redrawn every second, which follows the size of its
# window.

canvas .c -background white -highlightthickness 0
pack .c -fill both -expand 1

proc hand {angle length width color} {
    set w [winfo width .c]
    set h [winfo height .c]
    set r [expr {min($w, $h) / 2.0 * 0.9}]
    set a [expr {($angle - 90) * acos(-1) / 180}]
    .c create line [expr {$w / 2.0}] [expr {$h / 2.0}] \
	[expr {$w / 2.0 + cos($a) * $r * $length}] \
	[expr {$h / 2.0 + sin($a) * $r * $length}] \
	-width $width -fill $color -capstyle round
}

proc tick {} {
    .c delete all
    set w [winfo width .c]
    set h [winfo height .c]
    set r [expr {min($w, $h) / 2.0 * 0.9}]
    .c create oval [expr {$w / 2.0 - $r}] [expr {$h / 2.0 - $r}] \
	[expr {$w / 2.0 + $r}] [expr {$h / 2.0 + $r}] -width 4
    for {set i 0} {$i < 60} {incr i 5} {
	hand [expr {$i * 6}] 1.0 2 gray60
    }
    lassign [clock format [clock seconds] -format {%H %M %S}] hh mm ss
    scan "$hh $mm $ss" %d%d%d hh mm ss
    hand [expr {($hh % 12 + $mm / 60.0) * 30}] 0.5 8 black
    hand [expr {($mm + $ss / 60.0) * 6}] 0.75 5 black
    hand [expr {$ss * 6}] 0.85 2 red
    after 1000 tick
}
bind .c <Configure> {after cancel tick; tick}
