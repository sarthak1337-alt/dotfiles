# Sakura Line - rsync watchers.

# rsw <source> <destination>: keep destination in sync with source in the
# background, re-syncing on every change. The destination can be remote
# (host:path); one SSH connection is reused, so you authenticate once.
rsw() {
  (( $# == 2 )) || { echo "usage: rsw <source> <destination>"; return 1; }
  local src="${1%/}" dest="$2" sockets="${XDG_RUNTIME_DIR:-$HOME/.ssh}"
  local rsh="ssh -o ControlMaster=auto -o ControlPath=$sockets/rsw-%r@%h:%p -o ControlPersist=yes"
  setsid --fork env RSYNC_RSH="$rsh" bash -c '
    rsync -a "$1/" "$2"
    while inotifywait -r -q -e modify,create,delete,move "$1" >/dev/null; do rsync -a "$1/" "$2"; done
  ' rsw-watch "$src" "$dest" >/dev/null 2>&1
  echo "watching $src -> $dest"
}

# lsw: list watchers.   dsw: stop them all.
lsw() {
  local pid cmd rest found=0
  while read -r pid cmd; do
    rest="${cmd##*rsw-watch }"
    echo "$pid: ${rest% *} -> ${rest##* }"
    found=1
  done < <(pgrep -af 'rsw-watch ')
  (( found )) || echo "no active watchers"
}

dsw() {
  local pid found=0
  for pid in $(pgrep -f 'rsw-watch '); do
    kill -- -"$pid" 2>/dev/null && echo "stopped $pid" && found=1
  done
  (( found )) || echo "no active watchers"
}
