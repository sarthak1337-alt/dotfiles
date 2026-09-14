# Sakura Line - everyday functions.

# ff: fuzzy-find a file below the current directory, with a preview (pictures
# show inline in st and kitty). eff opens the pick in $EDITOR.
ff()  { fzf --preview 'rice-preview {}' --preview-window 'right,55%' "$@"; }
eff() { local f; f=$(ff) && [[ -n $f ]] && "$EDITOR" "$f"; }

# img <picture>...: show pictures right in the terminal.
img() { local f; for f in "$@"; do rice-preview "$f"; done; }

# open <file|url>: open with the default app, detached from the terminal.
open() { xdg-open "$@" >/dev/null 2>&1 & disown; }

# n: nvim, on the current directory when given nothing.
n() { if (( $# == 0 )); then nvim .; else nvim "$@"; fi; }

# compress <file|dir>: make <name>.tar.gz next to it (decompress is tar -xzf).
compress() { tar -czf "${1%/}.tar.gz" "${1%/}"; }
