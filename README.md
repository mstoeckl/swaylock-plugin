# swaylock-plugin

This is a fork of [`swaylock`](https://github.com/swaywm/waylock), a screen
locking utility for Wayland compositors. With it, instead of displaying a fixed image or color on the screen, you can display the animated output from any wallpaper program that implements the `wlr-layer-shell-unstable-v1` protocol. All you have to do is run `swaylock --command 'my-wallpaper --flags'`, where `my-wallpaper`
is replaced by your desired program. Programs which are known to work include:

* [`swaybg`](https://github.com/swaywm/swaybg), which displays regular background images
* [`mpvpaper`](https://github.com/GhostNaN/mpvpaper), which lets you play videos
* [`glpaper`](https://hg.sr.ht/~scoopta/glpaper), for custom shaders
* [`shaderbg`](https://git.sr.ht/~mstoeckl/shaderbg), also renders OpenGL shaders
* [`rwalkbg`](https://git.sr.ht/~mstoeckl/rwalkbg), a very slow animation
* [`wscreensaver`](https://git.sr.ht/~mstoeckl/wscreensaver), an experiment in porting a few xscreensaver hacks to Wayland

It is compatible with Wayland compositors that implement the `ext-session-lock-v1`
protocol.

See the man page, `swaylock(1)`, for instructions on using swaylock.

This is experimental software, so if something fails to work it's probably a bug
in this program -- report it at https://github.com/mstoeckl/swaylock-plugin .

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

On systems without PAM, you need to suid the swaylock binary:

    sudo chmod a+s /usr/local/bin/swaylock

Swaylock will drop root permissions shortly after startup.
