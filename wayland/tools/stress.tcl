# stress.tcl --
#
#	Random sequences of window operations, to find crashes of the Wayland
#	port and protocol errors. Usage: wish stress.tcl ?iterations? ?seed?
#	The seed is printed, so that a failing sequence can be replayed.

set iterations [expr {$argc > 0 ? [lindex $argv 0] : 2000}]
set seed [expr {$argc > 1 ? [lindex $argv 1] : [clock milliseconds] % 100000}]
expr {srand($seed)}
puts "seed $seed"

proc pick {list} {
    lindex $list [expr {int(rand() * [llength $list])}]
}
proc tops {} {
    lmap w [winfo children .] {
	if {[winfo class $w] eq "Toplevel"} {set w} continue
    }
}

set n 0
set ops {
    create destroy withdraw deiconify iconify resize title override
    menu unpost cascade tooltip alpha cursor update idle raise focus
    iconphoto transient text
}
image create photo icon -width 16 -height 24
icon put red -to 0 0 16 24

for {set i 0} {$i < $iterations} {incr i} {
    set op [pick $ops]
    set t [pick [tops]]
    if {$t eq "" && $op ni {create update idle}} {
	set op create
    }
    if {$i % 200 == 0} {
	puts "$i: [llength [tops]] toplevels"
    }
    switch $op {
	create {
	    set t .t[incr n]
	    toplevel $t -bg [pick {red green blue yellow white}]
	    label $t.l -text "toplevel $n"
	    pack $t.l
	    menu $t.m -tearoff 0
	    $t.m add command -label One
	    $t.m add cascade -label More -menu [menu $t.m.c -tearoff 0]
	    $t.m.c add command -label Sub
	}
	destroy {destroy $t}
	withdraw {wm withdraw $t}
	deiconify {wm deiconify $t}
	iconify {catch {wm iconify $t}}
	resize {wm geometry $t [expr {50 + int(rand() * 400)}]x[expr {30 + int(rand() * 300)}]}
	title {wm title $t [string repeat [pick {a ✓ שלום \U1F600}] [expr {int(rand() * 2000)}]]}
	override {
	    wm overrideredirect $t [expr {![wm overrideredirect $t]}]
	    wm withdraw $t
	    wm deiconify $t
	}
	menu {catch {$t.m post [winfo rootx $t] [winfo rooty $t]}}
	unpost {catch {$t.m unpost}}
	cascade {
	    catch {
		$t.m post [winfo rootx $t] [winfo rooty $t]
		$t.m activate 1
		$t.m postcascade 1
	    }
	}
	tooltip {
	    set tip $t.tip[incr n]
	    toplevel $tip -bg lightyellow
	    wm overrideredirect $tip 1
	    after [expr {int(rand() * 200)}] [list destroy $tip]
	}
	alpha {catch {wm attributes $t -alpha [expr {rand()}]}}
	cursor {$t configure -cursor [pick {xterm watch hand2 none {} crosshair}]}
	update {update}
	idle {update idletasks}
	raise {raise $t}
	focus {focus -force $t}
	iconphoto {wm iconphoto $t icon}
	transient {
	    set other [pick [tops]]
	    if {$other ne $t} {catch {wm transient $t $other}}
	}
	text {
	    catch {destroy $t.x}
	    text $t.x -width 20 -height 3
	    $t.x insert end "abc שלום مرحبا\n\U1F600 ✓"
	    pack $t.x
	}
    }
    if {rand() < 0.3} {
	update
    }
}
update
foreach t [tops] {destroy $t}
toplevel .final
update
after 300
update
puts "survived $iterations operations, final window mapped: [winfo ismapped .final]"
exit
