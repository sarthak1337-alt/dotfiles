! Rendered by rice-theme from colors.toml - edit templates/Xresources.tpl instead.
! st reads these through its xresources patch; reopen st (or send it USR1) to pick them up.

*.foreground:  {{ fg }}
*.background:  {{ bg }}
*.cursorColor: {{ accent }}

*.color0:  {{ bg }}
*.color1:  {{ red }}
*.color2:  {{ green }}
*.color3:  {{ yellow }}
*.color4:  {{ blue }}
*.color5:  {{ magenta }}
*.color6:  {{ cyan }}
*.color7:  {{ fg_alt }}
*.color8:  {{ muted }}
*.color9:  {{ bright_red }}
*.color10: {{ bright_green }}
*.color11: {{ bright_yellow }}
*.color12: {{ bright_blue }}
*.color13: {{ bright_magenta }}
*.color14: {{ bright_cyan }}
*.color15: {{ fg }}

st.font:     {{ font }}:pixelsize=14:antialias=true:autohint=true
st.alpha:    {{ alpha }}
st.borderpx: 15

Xcursor.theme: {{ cursor_theme }}
Xcursor.size:  {{ cursor_size }}
Xft.antialias: 1
Xft.hinting:   1
Xft.hintstyle: hintslight
Xft.rgba:      rgb
