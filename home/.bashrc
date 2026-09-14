# Sakura Line - bash.
# Aliases and functions live in ~/.config/rice/bash/. Anything personal or
# machine-only (work aliases, SDK paths, tokens) goes in ~/.bashrc.local,
# which is not part of the repo.

export PATH="$HOME/.local/bin:$PATH"
[[ $- != *i* ]] && { [[ -f $HOME/.bashrc.local ]] && . "$HOME/.bashrc.local"; return; }

export EDITOR=nvim
export VISUAL=nvim
export BAT_THEME=ansi
export MANPAGER="sh -c 'col -bx | bat -l man -p'"
export MANROFFOPT="-c"

theme="${XDG_STATE_HOME:-$HOME/.local/state}/rice/theme"

# --- history ---
HISTSIZE=50000
HISTFILESIZE=50000
HISTCONTROL=ignoreboth:erasedups
shopt -s histappend checkwinsize globstar
PROMPT_COMMAND="history -a${PROMPT_COMMAND:+; $PROMPT_COMMAND}"

# --- completion ---
[[ -r /usr/share/bash-completion/bash_completion ]] && . /usr/share/bash-completion/bash_completion

# --- prompt: two lines in the theme's colours ---
if command -v starship >/dev/null; then
  export STARSHIP_CONFIG="$theme/starship.toml"
  eval "$(starship init bash)"
fi

# --- fzf: Ctrl+R history, Ctrl+T files, Alt+C directories, coloured like rofi ---
[[ -f $theme/fzf.sh ]] && . "$theme/fzf.sh"
[[ -r /usr/share/fzf/key-bindings.bash ]] && . /usr/share/fzf/key-bindings.bash
[[ -r /usr/share/fzf/completion.bash ]] && . /usr/share/fzf/completion.bash

# --- zoxide: `z` jumps to directories you have been in before ---
command -v zoxide >/dev/null && eval "$(zoxide init bash)"

# --- rice aliases and functions ---
for rc in "$HOME"/.config/rice/bash/*.bash; do
  [[ -r $rc ]] && . "$rc"
done
unset rc

[[ -f $HOME/.bashrc.local ]] && . "$HOME/.bashrc.local"
