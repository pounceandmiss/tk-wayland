# Finger painting on a canvas: drag to draw, pick a color below.

set color black
canvas .c -background white -highlightthickness 0
ttk::frame .colors
foreach c {black red green3 blue orange purple} {
    radiobutton .colors.$c -background $c -selectcolor $c -indicatoron 0 \
	-variable color -value $c -width 3 -height 1
    pack .colors.$c -side left -padx 4 -pady 4
}
ttk::button .colors.clear -text Clear -command {.c delete all}
pack .colors.clear -side right -padx 4
pack .colors -side bottom -fill x
pack .c -fill both -expand 1

# A finger dragged on a canvas scrolls it, unless the canvas takes the
# finger as a mouse.
if {[tk windowingsystem] in {android wayland}} {
    tk touch mode .c mouse
}
bind .c <Button-1> {set last [list %x %y]}
bind .c <B1-Motion> {
    .c create line {*}$last %x %y -fill $color -width 6 -capstyle round
    set last [list %x %y]
}
