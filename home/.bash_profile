# ~/.bash_profile
[[ -f ~/.bashrc ]] && . ~/.bashrc

# Auto-start the graphical session, but ONLY in the exact situation where it is
# safe to do so. Any one of these guards failing leaves you at a normal shell.
#   - not already inside X
#   - not an SSH session (never hijack a remote login)
#   - physically on VT 1
#   - no Xorg already running for this user
# NOTE: deliberately NOT `exec startx`. If X dies or fails to start we fall back
# to an interactive shell on tty1 instead of a login loop.
if [[ -z "$DISPLAY" && -z "$SSH_CONNECTION" && "$XDG_VTNR" == "1" ]]; then
    if ! pgrep -u "$USER" -x Xorg >/dev/null 2>&1; then
        startx
    fi
fi
