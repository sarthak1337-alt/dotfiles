; Rendered by rice-theme from colors.toml - edit templates/polybar-modules.ini.tpl instead.
; Polybar only resolves ${colors.x} when it is the whole value, so modules that
; colour part of their own text live here with the theme's colours written in.

[module/date]
type = internal/date
interval = 1
date = %a %d
time = %H:%M
label = "%{A1:rice-panel calendar:}%{A3:rice-reminder new:}%{F{{ accent }}}%{F-} %time% %{F{{ dim }}}|%{F-} %date%%{A}%{A}"
format-padding = 1

[module/cpu]
type = custom/script
exec = rice-sysinfo --cpu --normal-color '{{ accent }}' --critical-color '{{ urgent }}'
click-left = rice-panel power
format = "%{F{{ accent }}}CPU%{F-} <label>"
format-padding = 1
interval = 2

[module/ram]
type = custom/script
exec = rice-sysinfo --ram --normal-color '{{ yellow }}' --critical-color '{{ urgent }}'
click-left = rice-panel power
format = "%{F{{ yellow }}}RAM%{F-} <label>"
format-padding = 1
interval = 2

[module/gpu]
type = custom/script
exec = rice-sysinfo --gpu --normal-color '{{ magenta }}' --critical-color '{{ urgent }}'
click-left = rice-panel power
format = "%{F{{ magenta }}}GPU%{F-} <label>"
format-padding = 1
interval = 2
