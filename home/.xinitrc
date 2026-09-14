#!/bin/sh
# Sakura Line - X session: pick up the system's session setup (D-Bus and the
# systemd user environment), then hand over to bspwm.
if [ -d /etc/X11/xinit/xinitrc.d ]; then
  for f in /etc/X11/xinit/xinitrc.d/?*.sh; do
    [ -x "$f" ] && . "$f"
  done
  unset f
fi

exec bspwm
