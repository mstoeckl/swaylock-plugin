# swaylock-plugin

This is a fork of [`swaylock`](https://github.com/swaywm/swaylock), a screen
locking utility for Wayland compositors. With `swaylock-plugin`, you can for
your lockscreen background display the animated output from any wallpaper program
that implements the `wlr-layer-shell-unstable-v1` protocol. All you have to do
is run `swaylock-plugin --command 'my-wallpaper ...'`, where `my-wallpaper ...`
is replaced by your desired program. Examples:

* [`swaybg`](https://github.com/swaywm/swaybg), which displays regular background images
* [`mpvpaper`](https://github.com/GhostNaN/mpvpaper), which lets you play videos
* [`shaderbg`](https://git.sr.ht/~mstoeckl/shaderbg), renders OpenGL shaders
* [`rwalkbg`](https://git.sr.ht/~mstoeckl/rwalkbg), a very slow animation
* [`wscreensaver`](https://git.sr.ht/~mstoeckl/wscreensaver), an experiment in porting a few xscreensaver hacks to Wayland
* You can rotate between wallpapers in a folder by setting the following script
  as the command; e.g.: `swaylock-plugin --command './rotate_example.sh /path/to/folder'`. (This works by periodically killing the wallpaper program, after which `swaylock-plugin` automatically restarts it.)
    ```
    #!/bin/sh
    file=`ls $1 | shuf -n 1`
    delay=60.
    echo "Runnning swaybg for $delay secs on: $1/$file"
    timeout $delay swaybg -i $1/$file
    ```

` swaylock-plugin` requires that the Wayland compositor implement the `ext-session-lock-v1` protocol.

This is experimental software, so if something fails to work it's probably a bug
in this program -- report it at https://github.com/mstoeckl/swaylock-plugin .

As this fork is not nearly as well tested as the original swaylock, before using this
program, ensure that you can recover from both an unresponsive lockscreen and one
that has crashed. (For example, in Sway, by creating a `--locked` bindsym to kill and
restart swaylock-plugin; or by switching to a different virtual terminal, running
`killall swaylock-plugin` and running swaylock-plugin, and restarting with e.g. `WAYLAND_DISPLAY=wayland-1 swaylock-plugin` .)

See the man page, [`swaylock-plugin(1)`](swaylock.1.scd), for instructions on using swaylock-plugin.

## Installation

Install dependencies:

* meson \*
* wayland
* wayland-protocols \*
* libxkbcommon
* cairo
* gdk-pixbuf2
* pam (optional)
* [scdoc](https://git.sr.ht/~sircmpwn/scdoc) (optional: man pages) \*
* git \*
* swaybg

_\* Compile-time dep_  

Run these commands:

    meson build
    ninja -C build
    sudo ninja -C build install

On systems without PAM, you need to suid the swaylock-plugin binary:

    sudo chmod a+s /usr/local/bin/swaylock-plugin

Swaylock will drop root permissions shortly after startup.
