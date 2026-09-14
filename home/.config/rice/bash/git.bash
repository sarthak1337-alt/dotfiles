# Sakura Line - git worktree helpers.

# ga <branch>: new branch in its own worktree next to the repository
# (../<repo>-<branch>), and cd into it.
ga() {
  local branch="${1:-}" top main dir
  [[ -n $branch ]] || { echo "usage: ga <branch>"; return 1; }
  top=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "ga: not inside a git repository"; return 1; }
  main=$(dirname "$(git -C "$top" rev-parse --path-format=absolute --git-common-dir)")
  dir="$(dirname "$main")/$(basename "$main")-${branch//\//-}"
  git -C "$top" worktree add -b "$branch" "$dir" && cd "$dir"
}

# gd: remove the current worktree and its branch (asks first; never the main one).
gd() {
  local top main branch answer
  top=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "gd: not inside a git repository"; return 1; }
  main=$(git worktree list --porcelain | awk '/^worktree / { print $2; exit }')
  [[ $top != "$main" ]] || { echo "gd: this is the main worktree; not removing it"; return 1; }
  branch=$(git branch --show-current)
  read -rp "Remove worktree $top and branch $branch? [y/N] " answer
  [[ $answer == [yY]* ]] || return 1
  cd "$main" && git worktree remove "$top" && git branch -D "$branch"
}
