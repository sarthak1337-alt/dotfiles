# Rendered by rice-theme from colors.toml - edit templates/fzf.sh.tpl instead.
# Sourced by ~/.bashrc so fzf, ff and Ctrl+R pickers match rofi.
export FZF_DEFAULT_OPTS="\
--color=bg:-1,bg+:{{ selection }},fg:{{ fg_alt }},fg+:{{ fg }},hl:{{ accent }},hl+:{{ accent }} \
--color=border:{{ border }},header:{{ accent }},info:{{ dim }},marker:{{ green }},pointer:{{ urgent }},prompt:{{ urgent }},spinner:{{ cyan }} \
--prompt='  ' --pointer='▌' --marker='+' --border=rounded --layout=reverse --height=60% --info=inline-right"
