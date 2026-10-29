# Edit this script and tap Run: its window comes up above the editor, and
# the back button closes it. What it prints is in the Console tab.

ttk::label .l -text "Hello, world!" -font {Helvetica 24}
ttk::button .b -text "Say hello" -command {
    puts "Hello at [clock format [clock seconds] -format %T]"
}
ttk::button .q -text Close -command exit
pack .l .b .q -padx 20 -pady 10
