#!/bin/sh
# Rendered by rice-theme from colors.toml - edit templates/bspwm-colors.sh.tpl instead.
bspc config focused_border_color  "{{ accent }}"
bspc config active_border_color   "{{ border }}"
bspc config normal_border_color   "{{ surface }}"
bspc config presel_feedback_color "{{ magenta }}"
bspc config border_width          {{ window_border }}
bspc config window_gap            {{ gap }}
