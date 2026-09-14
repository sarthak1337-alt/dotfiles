# Rendered by rice-theme from colors.toml - edit templates/starship.toml.tpl instead.
# One line, like bash's user@host prompt with colour and git:
#   sarthak@arch ~/rice  main !? ❯
"$schema" = "https://starship.rs/config-schema.json"
add_newline = false
command_timeout = 300
palette = "rice"

format = "$username$hostname$directory$git_branch$git_state$git_status$jobs$cmd_duration$character"

[palettes.rice]
fg      = "{{ fg }}"
dim     = "{{ dim }}"
accent  = "{{ accent }}"
urgent  = "{{ urgent }}"
green   = "{{ green }}"
yellow  = "{{ yellow }}"
magenta = "{{ magenta }}"

[username]
show_always = true
format      = "[$user]($style)"
style_user  = "bold accent"
style_root  = "bold urgent"

[hostname]
ssh_only = false
trim_at  = "."
format   = "[@](dim)[$hostname]($style) "
style    = "bold magenta"

[directory]
style             = "bold fg"
truncation_length = 3
truncate_to_repo  = false
truncation_symbol = "…/"
read_only         = " 󰌾"
read_only_style   = "urgent"
format            = "[$path]($style)[$read_only]($read_only_style) "

[git_branch]
symbol = " "
style  = "dim"
format = "[$symbol$branch]($style) "

[git_state]
style  = "yellow"
format = "[$state( $progress_current/$progress_total)]($style) "

[git_status]
style      = "yellow"
format     = "([$all_status$ahead_behind]($style) )"
conflicted = "="
untracked  = "?"
stashed    = ""
modified   = "!"
staged     = "+"
renamed    = "»"
deleted    = "✘"
ahead      = "⇡"
behind     = "⇣"
diverged   = "⇕"

[jobs]
symbol = "✦"
style  = "dim"
format = "[$symbol$number]($style) "

[cmd_duration]
min_time = 2000
format   = "[$duration](dim) "

[character]
success_symbol = "[❯](bold accent)"
error_symbol   = "[❯](bold urgent)"
vimcmd_symbol  = "[❮](bold green)"
