# st/config.sed - the rice's changes to st-flexipatch's config.def.h.
# The font and colours here are only fallbacks: rice-theme sets both through
# Xresources, and st re-reads them on USR1.

s|^static char \*font = .*|static char *font = "JetBrainsMono Nerd Font:pixelsize=14:antialias=true:autohint=true";|
s|^static int borderpx = .*|static int borderpx = 15;|
s|^float alpha = .*|float alpha = 0.75;|
s|^static unsigned int cursorshape = .*|static unsigned int cursorshape = 2;|

# Alt-based zoom, scrolling and transparency, as in the previous st build.
/^static Shortcut shortcuts\[\] = {/a\
	{ MODKEY,               XK_comma,       zoom,            {.f = +1} },\
	{ MODKEY,               XK_period,      zoom,            {.f = -1} },\
	{ MODKEY,               XK_g,           zoomreset,       {.f =  0} },\
	{ MODKEY,               XK_Page_Up,     kscrollup,       {.i = -1}, S_PRI },\
	{ MODKEY,               XK_Page_Down,   kscrolldown,     {.i = -1}, S_PRI },\
	{ MODKEY,               XK_k,           kscrollup,       {.i =  1}, S_PRI },\
	{ MODKEY,               XK_j,           kscrolldown,     {.i =  1}, S_PRI },\
	{ MODKEY,               XK_Up,          kscrollup,       {.i =  1}, S_PRI },\
	{ MODKEY,               XK_Down,        kscrolldown,     {.i =  1}, S_PRI },\
	{ MODKEY,               XK_u,           kscrollup,       {.i = -1}, S_PRI },\
	{ MODKEY,               XK_d,           kscrolldown,     {.i = -1}, S_PRI },\
	{ MODKEY,               XK_a,           changealpha,     {.f = +0.05} },\
	{ MODKEY,               XK_s,           changealpha,     {.f = -0.05} },
