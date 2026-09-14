# Rendered by rice-theme from colors.toml - edit templates/dunst.conf.tpl instead.
# Loaded as a drop-in (~/.config/dunst/dunstrc.d/50-theme.conf), so it only
# carries colours; layout and behaviour live in dunstrc.

[global]
    font = "{{ font }} 10"
    icon_theme = "{{ icon_theme }}, Adwaita, hicolor"

[urgency_low]
    background  = "{{ bg }}{{ alpha_hex }}"
    foreground  = "{{ fg_alt }}"
    frame_color = "{{ border }}"
    highlight   = "{{ dim }}"

[urgency_normal]
    background  = "{{ bg }}{{ alpha_hex }}"
    foreground  = "{{ fg }}"
    frame_color = "{{ border }}"
    highlight   = "{{ accent }}"

[urgency_critical]
    background  = "{{ bg }}{{ alpha_hex }}"
    foreground  = "{{ fg }}"
    frame_color = "{{ urgent }}"
    highlight   = "{{ urgent }}"

[osd]
    frame_color = "{{ accent }}"
    highlight   = "{{ accent }}"
