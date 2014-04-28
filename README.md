Userspace drum kit driver
========================

Just bought for fun one of those drum kits in nearby thrift shop. Wrote
this program because all existing ones (`drumroll` and `jackdrum`) didn't
work with my drum kit, and were using `libusb-0.1` which is deprecated, and 
didn't have loop buffer which is cool, and one of them used `math.h` only to 
do `pow(x,2)` but not simply `x << 1`.

All info necessary to determine endpoint got from `lsusb`. Some things may 
differ between different drum kits.

Usage
-----

To gracefully exit program send it either `SIGINT`, `SIGHUP`, `SIGTERM` or
`SIGQUIT`, eg. press <kbd>Ctrl+C</kbd> in your terminal.

Larger values of loop buffer will increase time spent at gathering data from 
drums, so it can minimize false release action. Big values will cause specific 
delay.

To check how note number and actual drum sound corresponds look at
<http://www.midi.org/techspecs/gm1sound.php>

Pad numbers in my case was:

    1 3 2
    6 5 4
