#!/usr/bin/env bash
# Sakura Line - install the rice on Arch Linux.
#
#   curl -fsSL https://rice.srtk.in/install.sh | bash
#   ~/rice/install.sh              full install (safe to run again)
#   ~/rice/install.sh --update     pull, re-link, re-render, rebuild what changed; asks nothing
#   ~/rice/install.sh --dry-run    show what would happen, change nothing
#   ~/rice/install.sh --skip-packages   everything is installed already; no pacman
#
# Installs the packages (pacman, plus paru for the AUR), links every file in
# home/ into $HOME and every script in bin/ into ~/.local/bin, renders the
# theme, builds st with sixel images, and offers calendar sync and a boot
# splash. Anything it would replace is moved to ~/.rice-backup-<date>/ first.
set -uo pipefail

RICE_REMOTE="${RICE_REMOTE:-https://github.com/sarthak1337-alt/dotfiles.git}"
RICE_HOME="${RICE_HOME:-$HOME/rice}"
STATE="${XDG_STATE_HOME:-$HOME/.local/state}/rice"
BACKUP="$HOME/.rice-backup-$(date +%Y%m%d-%H%M%S)"
DRY=0
UPDATE=0
SKIP_PACKAGES=0
FAILED=()

for arg in "$@"; do
  case "$arg" in
    --update)        UPDATE=1 ;;
    --dry-run|-n)    DRY=1 ;;
    --skip-packages) SKIP_PACKAGES=1 ;;
    -h|--help)       sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $arg (try --help)"; exit 2 ;;
  esac
done

c_hd=$'\e[1;38;2;146;185;234m'; c_ok=$'\e[38;2;143;174;116m'; c_wa=$'\e[38;2;216;168;124m'
c_er=$'\e[38;2;207;106;128m';   c_dim=$'\e[38;2;127;119;150m'; c_off=$'\e[0m'
say()  { printf '\n%s>>%s %s\n' "$c_hd" "$c_off" "$*"; }
ok()   { printf '  %s+%s %s\n' "$c_ok" "$c_off" "$*"; }
warn() { printf '  %s!%s %s\n' "$c_wa" "$c_off" "$*"; }
fail() { printf '  %sx%s %s\n' "$c_er" "$c_off" "$*"; FAILED+=("$*"); }
run()  { if (( DRY )); then printf '    %s$ %s%s\n' "$c_dim" "$*" "$c_off"; else "$@"; fi; }
ask()  { (( UPDATE )) && return 1; local a=""; { read -rp "  $1 [y/N] " a </dev/tty; } 2>/dev/null || return 1; [[ $a == [yY]* ]]; }

(( $(id -u) != 0 )) || { echo "Run this as your normal user; it asks for sudo when it needs it."; exit 1; }
command -v pacman >/dev/null || { echo "This rice installs on Arch Linux (pacman not found)."; exit 1; }

# ------------------------------------------------------------ find the repo
# Piped from curl there is no checkout yet: clone it, then run its own copy.
self_dir="$(cd "$(dirname "${BASH_SOURCE[0]:-.}")" 2>/dev/null && pwd)"
if [[ -f $self_dir/packages/pacman.txt && -d $self_dir/home ]]; then
  RICE_DIR="$self_dir"
else
  command -v git >/dev/null || sudo pacman -S --needed --noconfirm git
  if [[ -d $RICE_HOME/.git ]]; then
    git -C "$RICE_HOME" pull --ff-only
  else
    git clone "$RICE_REMOTE" "$RICE_HOME"
  fi
  exec bash "$RICE_HOME/install.sh" "$@" </dev/tty
fi
export PATH="$HOME/.local/bin:$PATH"

# ---------------------------------------------------------------- steps
update_repo() {
  (( UPDATE )) || return 0
  say "updating the rice"
  if [[ -n $(git -C "$RICE_DIR" status --porcelain) ]]; then
    warn "you have local changes in $RICE_DIR, so it was not pulled"
  else
    run git -C "$RICE_DIR" pull --ff-only || fail "git pull"
  fi
}

packages() {
  say "packages"
  local repo aur helper tmp
  mapfile -t repo < <(sed 's/#.*//' "$RICE_DIR/packages/pacman.txt" | awk 'NF { print $1 }')
  mapfile -t aur  < <(sed 's/#.*//' "$RICE_DIR/packages/aur.txt"    | awk 'NF { print $1 }')

  run sudo pacman -Syu --needed --noconfirm "${repo[@]}" || fail "pacman could not install everything"

  helper=$(command -v paru || command -v yay || true)
  if [[ -z $helper ]]; then
    warn "no AUR helper; building paru"
    tmp=$(mktemp -d)
    if run git clone --depth 1 https://aur.archlinux.org/paru-bin.git "$tmp/paru-bin" &&
       (cd "$tmp/paru-bin" && run makepkg -si --noconfirm); then
      helper=$(command -v paru || true)
    else
      fail "could not build paru; AUR packages skipped"
    fi
    rm -rf "$tmp"
  fi
  [[ -n $helper ]] && { run "$helper" -S --needed --noconfirm "${aur[@]}" || fail "some AUR packages failed"; }
  ok "packages done"
}

# link_one <source> <target>: symlink, moving whatever was there into the backup.
link_one() {
  local src="$1" dst="$2" rel
  [[ -L $dst && $(readlink "$dst") == "$src" ]] && return 0
  if [[ -e $dst || -L $dst ]]; then
    rel="${dst#"$HOME"/}"
    run mkdir -p "$BACKUP/$(dirname "$rel")"
    run mv "$dst" "$BACKUP/$rel"
    warn "moved your ~/$rel to the backup"
  fi
  run mkdir -p "$(dirname "$dst")"
  run ln -s "$src" "$dst"
}

# A previous dotfiles setup kept its own installer in ~/.config/rice, which is
# where this rice keeps your hooks and local settings. Move those files aside.
legacy_rice() {
  local old="$HOME/.config/rice" f moved=0
  for f in install.sh README.md packages.txt screenshot.webp particles st; do
    [[ -e $old/$f && ! -L $old/$f ]] || continue
    run mkdir -p "$BACKUP/.config/rice"
    run mv "$old/$f" "$BACKUP/.config/rice/$f"
    moved=1
  done
  (( moved )) && warn "moved the previous rice installer out of ~/.config/rice into the backup"
  return 0
}

# Swap the running session over: stop the old bar, notifications, compositor
# and key daemon, start the rice's, and re-run bspwmrc. Windows stay open.
apply_now() {
  [[ -n ${DISPLAY:-} ]] && pgrep -x bspwm >/dev/null || return 1
  ask "Apply it to this session now? The bar, notifications, picom and sxhkd restart; windows stay open." || return 1
  say "applying to this session"
  run rice-session restart && run bspc wm -r && ok "applied" || fail "could not restart the session daemons"
}

link_home() {
  say "linking configs and scripts"
  local f t="$STATE/theme"
  # bspwm runs bspwmrc as a program; without the executable bit it silently
  # starts with no settings, rules or session.
  run chmod +x "$RICE_DIR/home/.config/bspwm/bspwmrc" "$RICE_DIR"/bin/*
  while IFS= read -r -d '' f; do
    link_one "$f" "$HOME/${f#"$RICE_DIR"/home/}"
  done < <(find "$RICE_DIR/home" \( -type f -o -type l \) -print0)
  for f in "$RICE_DIR"/bin/*; do
    link_one "$f" "$HOME/.local/bin/$(basename "$f")"
  done
  # These point at the rendered theme, which rice-theme rewrites on every change.
  link_one "$t/dunst.conf"      "$HOME/.config/dunst/dunstrc.d/50-theme.conf"
  link_one "$t/eww-colors.scss" "$HOME/.config/eww/colors.scss"
  link_one "$t/gtk.css"         "$HOME/.config/gtk-3.0/gtk.css"
  link_one "$t/gtk.css"         "$HOME/.config/gtk-4.0/gtk.css"
  link_one "$t/btop.theme"      "$HOME/.config/btop/themes/rice.theme"
  link_one "$t/imv-config"      "$HOME/.config/imv/config"
  [[ -d $BACKUP ]] && warn "replaced files are in $BACKUP"
  ok "linked"
}

local_files() {
  say "your own settings"
  local hooks="$HOME/.config/rice/hooks" e
  for e in post-boot theme-set font-set wallpaper-set lock unlock update; do run mkdir -p "$hooks/$e.d"; done
  if [[ ! -f $HOME/.bashrc.local ]] && (( ! DRY )); then
    printf '%s\n' "# Personal shell settings: aliases, exports, SDK paths. Not part of the rice repo." > "$HOME/.bashrc.local"
    ok "created ~/.bashrc.local"
  fi
  if [[ ! -f $HOME/.config/rice/local.sh ]] && (( ! DRY )); then
    printf '%s\n' "# Machine-specific bspwm extras, sourced by bspwmrc: apps to autostart," \
      "# per-app desktop rules, monitor layouts. Not part of the rice repo." > "$HOME/.config/rice/local.sh"
    ok "created ~/.config/rice/local.sh"
  fi
  ok "hooks live in ~/.config/rice/hooks/<event>.d/"
}

theme() {
  say "theme"
  local name; name=$(rice-theme current)
  if [[ -n ${DISPLAY:-} ]] && pgrep -x bspwm >/dev/null; then
    run rice-theme reload && ok "re-rendered $name and reloaded running apps" || fail "theme reload"
  else
    run rice-theme render "$name" && ok "rendered $name" || fail "theme render"
  fi
}

build_st() {
  say "st, with sixel images"
  local stamp="$STATE/st.built" want
  want=$(cat "$RICE_DIR"/st/build.sh "$RICE_DIR"/st/patches.list "$RICE_DIR"/st/config.sed | sha1sum | cut -c1-16)
  if [[ -f $stamp && $(cat "$stamp") == "$want" ]] && command -v st >/dev/null; then
    ok "st is up to date"; return
  fi
  if run bash "$RICE_DIR/st/build.sh" --install; then
    (( DRY )) || { mkdir -p "$STATE"; echo "$want" > "$stamp"; }
    ok "st installed"
  else
    fail "st build"
  fi
}

build_particles() {
  say "particles wallpaper layer"
  if run make -s -C "$RICE_DIR/particles" && run sudo make -s -C "$RICE_DIR/particles" install; then
    ok "particles installed"
  else
    fail "particles build"
  fi
}

system_setup() {
  say "services and hardware"
  run sudo systemctl enable --now NetworkManager.service bluetooth.service power-profiles-daemon.service ||
    warn "some services did not start (normal without that hardware)"
  if [[ ! -f /etc/modules-load.d/i2c-dev.conf ]]; then
    run sudo sh -c 'echo i2c-dev > /etc/modules-load.d/i2c-dev.conf' && run sudo modprobe i2c-dev &&
      ok "i2c-dev loads at boot (monitor brightness over DDC)"
  fi
  if getent group i2c >/dev/null && ! id -nG | grep -qw i2c; then
    run sudo usermod -aG i2c "$USER" && warn "added you to the i2c group; it applies at your next login"
  fi
}

calendar() {
  say "calendar"
  if [[ -f ${XDG_CONFIG_HOME:-$HOME/.config}/khal/config ]]; then ok "already set up"; return; fi
  if ask "Sync the calendar with a CalDAV server (Baikal, Nextcloud, Fastmail, ...)?"; then
    run rice-cal setup || fail "calendar setup"
  else
    warn "skipped; run 'rice-cal setup' whenever you like"
  fi
}

boot_splash() {
  say "boot splash"
  if grep -Eq '^HOOKS=.*\bplymouth\b' /etc/mkinitcpio.conf 2>/dev/null; then ok "plymouth is already in the initramfs"; return; fi
  ask "Show Plymouth's stock splash at boot? This edits mkinitcpio.conf and the kernel options." || { warn "skipped"; return; }

  run sudo cp /etc/mkinitcpio.conf /etc/mkinitcpio.conf.rice-backup
  run sudo sed -i -E '/^HOOKS=/ s/\b(systemd|udev)\b/\1 plymouth/' /etc/mkinitcpio.conf

  if [[ -f /etc/default/grub ]]; then
    run sudo cp /etc/default/grub /etc/default/grub.rice-backup
    grep -q 'splash' /etc/default/grub ||
      run sudo sed -i -E 's/^(GRUB_CMDLINE_LINUX_DEFAULT="[^"]*)"/\1 quiet splash"/' /etc/default/grub
    run sudo grub-mkconfig -o /boot/grub/grub.cfg >/dev/null || fail "grub-mkconfig"
  elif compgen -G "/boot/loader/entries/*.conf" >/dev/null; then
    local entry
    for entry in /boot/loader/entries/*.conf; do
      grep -q splash "$entry" || run sudo sed -i -E '/^options / s/$/ quiet splash/' "$entry"
    done
  else
    warn "add 'quiet splash' to your kernel command line by hand"
  fi
  run sudo mkinitcpio -P >/dev/null && ok "plymouth enabled from the next boot" || fail "mkinitcpio"
}

# ----------------------------------------------------------------- main
printf '\n%s  Sakura Line%s  rice installer · %s\n' "$c_hd" "$c_off" "$RICE_DIR"
(( DRY )) && warn "dry run: nothing will be changed"

update_repo
(( SKIP_PACKAGES )) || packages
legacy_rice
link_home
local_files
theme
build_st
build_particles
system_setup
calendar
boot_splash
(( UPDATE )) && run rice-hook update

printf '\n'
if (( ${#FAILED[@]} == 0 )); then
  say "done"
else
  say "finished with ${#FAILED[@]} problem(s):"
  for f in "${FAILED[@]}"; do printf '    %s- %s%s\n' "$c_er" "$f" "$c_off"; done
fi
if apply_now; then
  printf '\n  %ssuper + k%s lists every shortcut.\n\n' "$c_hd" "$c_off"
elif [[ -n ${DISPLAY:-} ]] && pgrep -x bspwm >/dev/null; then
  printf '  To switch this session over later: %srice-session restart && bspc wm -r%s\n\n' "$c_hd" "$c_off"
else
  printf '  Log in on tty1 and the desktop starts; %ssuper + k%s lists every shortcut.\n\n' "$c_hd" "$c_off"
fi
