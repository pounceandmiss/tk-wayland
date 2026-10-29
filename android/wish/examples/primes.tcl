# A script without windows: its output goes to the Console tab. After it
# ran, the console evaluates commands in its interpreter: try "isprime 97".

proc isprime {n} {
    if {$n < 2} {return 0}
    for {set d 2} {$d * $d <= $n} {incr d} {
	if {$n % $d == 0} {return 0}
    }
    return 1
}

set primes {}
for {set n 2} {[llength $primes] < 50} {incr n} {
    if {[isprime $n]} {lappend primes $n}
}
puts "The first 50 primes:"
puts $primes
puts "Tcl [info patchlevel], Tk [package require Tk],\
	[tk windowingsystem], scale [tk scaling]"
