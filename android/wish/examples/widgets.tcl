# Some of the themed widgets, and their values in the console.

ttk::notebook .nb
pack .nb -fill both -expand 1 -padx 6 -pady 6

ttk::frame .nb.input -padding 10
.nb add .nb.input -text Input
ttk::label .nb.input.l -text "Name:"
ttk::entry .nb.input.e -textvariable name
ttk::checkbutton .nb.input.c -text "Subscribe" -variable subscribe
ttk::radiobutton .nb.input.r1 -text Small -variable size -value small
ttk::radiobutton .nb.input.r2 -text Large -variable size -value large
ttk::combobox .nb.input.cb -values {Tcl Tk Android Wayland} \
    -textvariable topic -state readonly
ttk::scale .nb.input.s -from 0 -to 100 -variable level
ttk::progressbar .nb.input.p -variable level
ttk::button .nb.input.b -text Show -command {
    puts [list name $name subscribe $subscribe size $size topic $topic \
	level [expr {round($level)}]]
}
set name ""; set subscribe 1; set size small; set topic Tk; set level 40
grid .nb.input.l .nb.input.e -sticky ew -pady 4
foreach w {c r1 r2 cb s p b} {
    grid .nb.input.$w - -sticky ew -pady 4
}
grid columnconfigure .nb.input 1 -weight 1

ttk::frame .nb.tree -padding 10
.nb add .nb.tree -text Tree
ttk::treeview .nb.tree.t -columns size -yscrollcommand {.nb.tree.y set}
ttk::scrollbar .nb.tree.y -command {.nb.tree.t yview}
.nb.tree.t heading #0 -text File
.nb.tree.t heading size -text Size
pack .nb.tree.y -side right -fill y
pack .nb.tree.t -fill both -expand 1
proc fill {parent dir} {
    foreach f [lsort [glob -nocomplain -directory $dir *]] {
	set id [.nb.tree.t insert $parent end -text [file tail $f] \
	    -values [list [expr {[file isdirectory $f] ? "" : [file size $f]}]]]
	if {[file isdirectory $f]} {fill $id $f}
    }
}
fill {} $tk_library
