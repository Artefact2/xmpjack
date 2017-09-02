xmpjack
=======

A very simple module player based on libxmp. Outputs audio samples to
JACK only.

Released under the WTFPLv2, see COPYING for the full license text.

~~~
Usage: xmpjack [options] [--] <modfiles...>

Options:
        --jack-client-name foo
                Use custom JACK client name (default xmpjack)
        -n, --jack-no-autoconnect
                Do not autoconnect to first available physical ports
        --jack-no-transport
                Do not rely on JACK transport for play/pause/seek
        --jack-connect-left foo, --jack-connect-right bar
                Connect to specified JACK ports before playback
        -l, --loop
                Enable looping of modules (default is no looping)
        -p, --paused
                Don't automatically start playback
        -s, --shuffle
                Play back modules in random order

Interactive commands:
        q       Quit the program
        SPC     Toggle play/pause
        n       Play next module
        p       Play previous module
        /*      Increase/decrease gain by 1 dB
        Up/Dn   Pattern seeking
~~~

Dependencies
------------

* JACK
* libxmp
* a VT100-compatible terminal

Demo (video)
------------

[![video thumbnail](https://i.ytimg.com/vi/S-SZB6avr6w/maxresdefault.jpg)](http://www.youtube.com/watch?v=S-SZB6avr6w)

Thanks to every contributor of both JACK and libxmp for producing
amazing, libre software.

* <https://github.com/cmatsuoka/libxmp>
* <http://jackaudio.org/>

Improvements
------------

* ~~Autoconnect toggle, better autoconnect code~~
* ~~Keyboard controls (pause, next/prev, toggle loop, etc.)~~
* ~~Shuffle mode~~
* Sequence support
* ~~Loop mode~~
* ~~Cool visualisation~~
* User-selectable interpolation, stereo separation, etc.
* ~~JACK transport (pause/play etc) support~~
* JACK freewheel support
* ~~Seeking~~ (partial, will always be flawed)
