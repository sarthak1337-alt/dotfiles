; Rendered by rice-theme from colors.toml - edit templates/polybar-colors.ini.tpl instead.
[colors]
bg       = {{ bg }}
bg-trans = #{{ alpha_hex }}{{ bg_strip }}
surface  = {{ surface }}
border   = #99{{ border_strip }}
text     = {{ fg }}
dim      = {{ dim }}
muted    = {{ muted }}
accent   = {{ accent }}
urgent   = {{ urgent }}
red      = {{ red }}
green    = {{ green }}
yellow   = {{ yellow }}
blue     = {{ blue }}
magenta  = {{ magenta }}
cyan     = {{ cyan }}

[fonts]
main  = {{ font }}:style=Bold:pixelsize=10;2
large = {{ font }}:style=Bold:pixelsize=12;2
