# Sakura Line - ssh helpers.

# ssh: when an interactive session drops, tidy the terminal and reconnect until
# you press Ctrl-C. Only a real dropped shell reconnects: a remote command,
# piped input, or a failure within the first 30 seconds (bad host, bad key)
# returns normally, so nothing is ever replayed by accident.
ssh() {
  local rc started=$SECONDS
  command ssh "$@"
  rc=$?
  [[ -t 1 ]] || return $rc
  _ssh_disarm

  if (( rc != 255 )) || [[ ! -t 0 ]] || ! _ssh_interactive "$@" || (( SECONDS - started < 30 )); then
    return $rc
  fi

  # A subshell, so Ctrl-C stops both the attempt and the loop.
  (
    while :; do
      echo "Connection lost. Reconnecting (Ctrl-C to stop)..."
      sleep 2
      command ssh "$@"
      rc=$?
      _ssh_disarm
      (( rc != 255 )) && exit $rc
    done
  )
}

# A connection that dies mid-session leaves the remote tmux or editor's
# terminal modes on (mouse reporting floods the prompt); switch them off.
_ssh_disarm() {
  printf '\e[?1000l\e[?1002l\e[?1003l\e[?1006l\e[?1004l\e[?1049l\e[?25h'
}

# True when the arguments open a shell: a destination and no remote command,
# counting a RemoteCommand set in ssh_config.
_ssh_interactive() {
  local value_opts="BbcDEeFIiJLlmOoPpQRSWw" argv=("$@") arg letters i dest="" opts_done=""
  while (( $# )); do
    arg="$1"; shift
    if [[ -z $opts_done && $arg == "--" ]]; then
      opts_done=1
    elif [[ -z $opts_done && $arg == -?* ]]; then
      letters="${arg#-}"
      for (( i = 0; i < ${#letters}; i++ )); do
        if [[ $value_opts == *"${letters:i:1}"* ]]; then
          (( i == ${#letters} - 1 )) && shift
          break
        fi
      done
    elif [[ -z $dest ]]; then
      dest="$arg"
    else
      return 1
    fi
  done
  [[ -n $dest ]] || return 1
  local resolved
  resolved=$(command ssh -G "${argv[@]}" 2>/dev/null) || return 1
  ! grep -i '^remotecommand ' <<<"$resolved" | grep -qvi '^remotecommand none$'
}

# fip <host> <port>...: forward remote ports to localhost (web dev against a remote box).
fip() {
  (( $# >= 2 )) || { echo "usage: fip <host> <port>..."; return 1; }
  local host="$1" port; shift
  for port in "$@"; do
    command ssh -f -N -L "$port:localhost:$port" "$host" && echo "localhost:$port -> $host:$port"
  done
}

# dip <port>...: stop those forwards.   lip: list active forwards.
dip() {
  (( $# >= 1 )) || { echo "usage: dip <port>..."; return 1; }
  local port
  for port in "$@"; do
    pkill -f "ssh.*-L $port:localhost:$port" && echo "stopped $port" || echo "no forward on $port"
  done
}
lip() { pgrep -af 'ssh.*-L [0-9]+:localhost:[0-9]+' || echo "no active forwards"; }
