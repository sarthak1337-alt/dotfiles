# Rendered by rice-theme from colors.toml - edit templates/imv-config.tpl instead.
# Linked as ~/.config/imv/config.
[options]
background = {{ bg_strip }}
overlay_font = {{ font }}:11
overlay_text_color = {{ fg_strip }}
overlay_background_color = {{ bg_strip }}
overlay_background_alpha = cc

[binds]
# Copy the picture to the clipboard
<Ctrl+c> = exec sh -c 'xclip -selection clipboard -t "$(file -b --mime-type "$1")" -i "$1"' _ "$imv_current_file"
# Make it the wallpaper
<Ctrl+w> = exec rice-wallpaper set "$imv_current_file"
# Rotate the file on disk by 90 degrees
<Ctrl+r> = exec mogrify -rotate 90 "$imv_current_file"
# Print it
<Ctrl+p> = exec lp "$imv_current_file"
# Move to the trash and quit / move to the trash and show the next one
<Ctrl+x> = exec gio trash -- "$imv_current_file"; quit
<Ctrl+Shift+X> = exec gio trash -- "$imv_current_file"; close
