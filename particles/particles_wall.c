/*
 * particle.X11 - an antialiased, click-through particle field that lives on the
 * X11 wallpaper layer.
 *
 * Design notes:
 *   - The window is a 32-bit ARGB, override-redirect window kept at the bottom
 *     of the stack and tagged _NET_WM_WINDOW_TYPE_DESKTOP, so a compositor
 *     (picom, xcompmgr, ...) blends it over the root wallpaper and every real
 *     window draws on top of it.
 *   - Its XFixes input region is empty, so clicks, scrolls and keys go straight
 *     through to whatever is underneath. No pointer/keyboard grabs, ever.
 *   - The cursor is read with XQueryPointer against the root window, so the
 *     particles keep reacting to it even while it is over other applications.
 *   - Frames are drawn with cairo into a MIT-SHM XImage (zero-copy present) and
 *     only the damaged horizontal bands are cleared and uploaded.
 *   - Without a compositor it falls back to compositing the root wallpaper
 *     pixmap into its own buffer, so it still looks right.
 *
 * Build:  make        (or see the command at the bottom of README.md)
 * License: MIT
 */

#define _GNU_SOURCE

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>

#include <cairo.h>
#include <cairo-xlib.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define APP_NAME    "particles"
#define APP_VERSION "2.0"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* rows per damage band; a band is the granularity of clear + upload */
#define BAND_H 16

/* number of pre-rendered dot sizes */
#define SPRITE_LEVELS 16

/* ------------------------------------------------------------------ config */

enum { CUR_NONE = 0, CUR_GRAB = 1, CUR_REPEL = 2, CUR_ATTRACT = 4 };
enum { EDGE_BOUNCE = 0, EDGE_WRAP = 1 };

typedef struct {
    int    count;             /* 0 => derive from density                     */
    double density;           /* particles per megapixel                      */
    double radius_min, radius_max;
    double speed;             /* px per second                                */
    double link_dist;
    double link_width;
    double link_opacity;
    double opacity;           /* particle core opacity                        */
    double glow;              /* halo radius as a multiple of particle radius */
    int    cursor_mode;       /* bitmask of CUR_*                             */
    double cursor_dist;       /* how far the grab links reach                 */
    double repel_dist;        /* push radius; 0 => 0.6 * cursor_dist          */
    double cursor_strength;   /* 1.0 pushes a particle exactly onto the rim   */
    double cursor_response;   /* how fast the push settles / releases, 1/s    */
    double cursor_ease;       /* cursor position smoothing, 1/s               */
    int    edges;             /* EDGE_BOUNCE | EDGE_WRAP                      */
    int    fps;
    double color[3];
    double link_color[3];
    int    keep_below;
    int    pause_when_covered;
    char   log_path[512];
} Config;

static void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->count        = 0;
    c->density      = 70.0;
    c->radius_min   = 1.1;
    c->radius_max   = 2.6;
    c->speed        = 26.0;
    c->link_dist    = 130.0;
    c->link_width   = 1.1;
    c->link_opacity = 0.55;
    c->opacity      = 0.85;
    c->glow         = 3.2;
    c->cursor_mode  = CUR_GRAB | CUR_REPEL;
    c->cursor_dist  = 170.0;
    c->repel_dist      = 0.0;       /* auto: 0.6 * cursor_dist */
    c->cursor_strength = 1.0;
    c->cursor_response = 9.0;
    c->cursor_ease     = 18.0;
    c->edges        = EDGE_BOUNCE;
    c->fps          = 60;
    c->color[0] = 0.86; c->color[1] = 0.91; c->color[2] = 1.0;   /* soft white-blue */
    c->link_color[0] = 0.68; c->link_color[1] = 0.82; c->link_color[2] = 1.0;
    c->keep_below         = 1;
    c->pause_when_covered = 1;
    c->log_path[0] = '\0';
}

static int parse_bool(const char *v, int *out)
{
    if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes") ||
        !strcasecmp(v, "on"))  { *out = 1; return 0; }
    if (!strcasecmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no") ||
        !strcasecmp(v, "off")) { *out = 0; return 0; }
    return -1;
}

static int parse_color(const char *v, double out[3])
{
    unsigned r, g, b;
    const char *s = (*v == '#') ? v + 1 : v;
    if (strlen(s) == 6 && sscanf(s, "%2x%2x%2x", &r, &g, &b) == 3) {
        out[0] = r / 255.0; out[1] = g / 255.0; out[2] = b / 255.0;
        return 0;
    }
    if (strlen(s) == 3 && sscanf(s, "%1x%1x%1x", &r, &g, &b) == 3) {
        out[0] = r / 15.0; out[1] = g / 15.0; out[2] = b / 15.0;
        return 0;
    }
    return -1;
}

static int parse_cursor_mode(const char *v, int *out)
{
    int mode = 0;
    char buf[128], *tok, *save = NULL;
    snprintf(buf, sizeof buf, "%s", v);
    for (tok = strtok_r(buf, ",+ ", &save); tok; tok = strtok_r(NULL, ",+ ", &save)) {
        if (!strcasecmp(tok, "none"))         mode  = 0;
        else if (!strcasecmp(tok, "grab"))    mode |= CUR_GRAB;
        else if (!strcasecmp(tok, "repel"))   mode |= CUR_REPEL;
        else if (!strcasecmp(tok, "attract")) mode |= CUR_ATTRACT;
        else if (!strcasecmp(tok, "both"))    mode |= CUR_GRAB | CUR_REPEL;
        else return -1;
    }
    if (mode & CUR_ATTRACT) mode &= ~CUR_REPEL;
    *out = mode;
    return 0;
}

/* Shared by the config file and the command line so both speak the same
 * vocabulary: "speed = 30" in the file is "--speed 30" on the CLI. */
static int set_option(Config *c, const char *key, const char *val)
{
    if (!val) return -1;
    if      (!strcmp(key, "count"))         c->count       = atoi(val);
    else if (!strcmp(key, "density"))       c->density     = atof(val);
    else if (!strcmp(key, "radius-min"))    c->radius_min  = atof(val);
    else if (!strcmp(key, "radius-max"))    c->radius_max  = atof(val);
    else if (!strcmp(key, "radius")) {
        double r = atof(val); c->radius_min = r * 0.6; c->radius_max = r * 1.4;
    }
    else if (!strcmp(key, "speed"))         c->speed        = atof(val);
    else if (!strcmp(key, "link-distance")) c->link_dist    = atof(val);
    else if (!strcmp(key, "link-width"))    c->link_width   = atof(val);
    else if (!strcmp(key, "link-opacity"))  c->link_opacity = atof(val);
    else if (!strcmp(key, "opacity"))       c->opacity      = atof(val);
    else if (!strcmp(key, "glow"))          c->glow         = atof(val);
    else if (!strcmp(key, "cursor-distance")) c->cursor_dist  = atof(val);
    else if (!strcmp(key, "repel-distance"))  c->repel_dist   = atof(val);
    else if (!strcmp(key, "cursor-strength")) c->cursor_strength = atof(val);
    else if (!strcmp(key, "cursor-response")) c->cursor_response = atof(val);
    else if (!strcmp(key, "fps"))           c->fps          = atoi(val);
    else if (!strcmp(key, "cursor"))        return parse_cursor_mode(val, &c->cursor_mode);
    else if (!strcmp(key, "color"))         return parse_color(val, c->color);
    else if (!strcmp(key, "link-color"))    return parse_color(val, c->link_color);
    else if (!strcmp(key, "keep-below"))    return parse_bool(val, &c->keep_below);
    else if (!strcmp(key, "pause-when-covered"))
                                            return parse_bool(val, &c->pause_when_covered);
    else if (!strcmp(key, "edges")) {
        if      (!strcasecmp(val, "bounce")) c->edges = EDGE_BOUNCE;
        else if (!strcasecmp(val, "wrap"))   c->edges = EDGE_WRAP;
        else return -1;
    }
    else if (!strcmp(key, "log"))
        snprintf(c->log_path, sizeof c->log_path, "%s", val);
    else return -2;                                     /* unknown key */
    return 0;
}

static void config_path(char *buf, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(buf, n, "%s/particles.conf", xdg);
    else if (home)   snprintf(buf, n, "%s/.config/particles.conf", home);
    else             snprintf(buf, n, "/dev/null");
}

static void load_config_file(Config *c, const char *path)
{
    char line[512];
    FILE *f = fopen(path, "r");
    int lineno = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *key, *val, *hash, *end;
        lineno++;
        if ((hash = strchr(line, '#'))) *hash = '\0';
        key = line;
        while (*key && isspace((unsigned char)*key)) key++;
        if (!*key) continue;
        val = strchr(key, '=');
        if (!val) continue;
        *val++ = '\0';
        end = key + strlen(key);
        while (end > key && isspace((unsigned char)end[-1])) *--end = '\0';
        while (*val && isspace((unsigned char)*val)) val++;
        end = val + strlen(val);
        while (end > val && isspace((unsigned char)end[-1])) *--end = '\0';
        if (set_option(c, key, val) != 0)
            fprintf(stderr, "%s: %s:%d: bad option '%s'\n", APP_NAME, path, lineno, key);
    }
    fclose(f);
}

/* ------------------------------------------------------------------- state */

typedef struct {
    float x, y;               /* drifting base position                       */
    float vx, vy;
    float ox, oy;             /* cursor displacement, spring-damped           */
    float sx, sy;             /* where it is actually drawn: base + offset    */
    float r;
    float alpha;              /* per-particle opacity jitter                  */
    short level;              /* index into App.sprite                        */
} Particle;

typedef struct {
    Display  *dpy;
    int       screen;
    Window    root, win;
    Visual   *visual;
    Colormap  cmap;
    int       depth;
    GC        gc;

    int       w, h;

    /* presentation buffer */
    XImage           *ximg;
    XShmSegmentInfo   shm;
    int               use_shm;
    unsigned char    *pixels;
    int               stride;
    cairo_surface_t  *surface;

    /* wallpaper copy, used only when nothing is compositing for us */
    unsigned char    *bg;
    int               composited;

    /* damage bands: x span per band, -1 == clean */
    int  bands;
    int *dmg_min, *dmg_max;       /* current frame  */
    int *prev_min, *prev_max;     /* previous frame */

    /* particles + uniform grid used for the link pass */
    Particle *p;
    int       n;

    /* pre-rendered A8 dot sprites, blitted as masks every frame */
    unsigned char *sprite[SPRITE_LEVELS];
    int            sprite_size[SPRITE_LEVELS];
    int            sprite_stride[SPRITE_LEVELS];
    double         sprite_c[SPRITE_LEVELS];     /* centre offset in px */
    int      *cell_of, *cell_start, *cell_cursor, *order;
    int       gw, gh;
    double    cell;

    /* cursor */
    double cx, cy;
    int    cursor_on;

    Atom atom_active, atom_state, atom_fullscreen;

    /* xrandr / event plumbing */
    int  rr_event_base, rr_error_base;
    int  has_randr;
    int  obscured;
    int  covered;

    Config cfg;
} App;

static volatile sig_atomic_t g_quit   = 0;
static volatile sig_atomic_t g_reload = 0;

static void on_signal(int sig)
{
    if (sig == SIGHUP) g_reload = 1;
    else               g_quit   = 1;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double frand(void) { return rand() / (double)RAND_MAX; }

static double frand_range(double a, double b) { return a + (b - a) * frand(); }

/* --------------------------------------------------------------- particles */

static void sprites_free(App *a)
{
    int i;
    for (i = 0; i < SPRITE_LEVELS; i++) { free(a->sprite[i]); a->sprite[i] = NULL; }
}

/* One alpha-only sprite per radius level: a solid antialiased core wrapped in a
 * soft halo. Drawn once here, then reused as a mask every frame. */
static void sprites_build(App *a)
{
    int i;
    sprites_free(a);
    for (i = 0; i < SPRITE_LEVELS; i++) {
        double r = SPRITE_LEVELS > 1
                 ? a->cfg.radius_min + (a->cfg.radius_max - a->cfg.radius_min)
                                        * i / (double)(SPRITE_LEVELS - 1)
                 : a->cfg.radius_min;
        double halo = r * a->cfg.glow;
        int size, c;
        cairo_t *cr;
        cairo_surface_t *surf;
        cairo_pattern_t *pat;

        if (halo < r + 0.75) halo = r + 0.75;
        size = 2 * (int)ceil(halo + 1.0) + 1;
        c    = size / 2;

        surf = cairo_image_surface_create(CAIRO_FORMAT_A8, size, size);
        cr   = cairo_create(surf);
        cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);   /* costs nothing, done once */
        pat = cairo_pattern_create_radial(c, c, 0, c, c, halo);
        cairo_pattern_add_color_stop_rgba(pat, 0.0,                             1, 1, 1, 1.00);
        cairo_pattern_add_color_stop_rgba(pat, (r * 0.72) / halo,               1, 1, 1, 1.00);
        cairo_pattern_add_color_stop_rgba(pat, r / halo,                        1, 1, 1, 0.42);
        cairo_pattern_add_color_stop_rgba(pat, (r + (halo - r) * 0.45) / halo,  1, 1, 1, 0.10);
        cairo_pattern_add_color_stop_rgba(pat, 1.0,                             1, 1, 1, 0.00);
        cairo_set_source(cr, pat);
        cairo_arc(cr, c, c, halo, 0, 2.0 * M_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
        cairo_destroy(cr);
        cairo_surface_flush(surf);

        {
            int st = cairo_image_surface_get_stride(surf);
            const unsigned char *src = cairo_image_surface_get_data(surf);
            a->sprite[i]        = malloc((size_t)st * size);
            a->sprite_size[i]   = size;
            a->sprite_stride[i] = st;
            a->sprite_c[i]      = c;
            if (a->sprite[i]) memcpy(a->sprite[i], src, (size_t)st * size);
        }
        cairo_surface_destroy(surf);
    }
}

static void particles_init(App *a)
{
    int i;
    int n = a->cfg.count > 0
          ? a->cfg.count
          : (int)(a->cfg.density * ((double)a->w * a->h) / 1.0e6 + 0.5);

    if (n < 1)     n = 1;
    if (n > 20000) n = 20000;

    free(a->p);
    a->p = calloc(n, sizeof *a->p);
    a->n = n;

    for (i = 0; i < n; i++) {
        Particle *p = &a->p[i];
        double ang  = frand() * 2.0 * M_PI;
        double spd  = a->cfg.speed * frand_range(0.45, 1.15);
        p->x  = frand() * a->w;
        p->y  = frand() * a->h;
        p->vx = (float)(cos(ang) * spd);
        p->vy = (float)(sin(ang) * spd);
        p->r  = (float)frand_range(a->cfg.radius_min, a->cfg.radius_max);
        {
            double span = a->cfg.radius_max - a->cfg.radius_min;
            int lv = span > 1e-6
                   ? (int)((p->r - a->cfg.radius_min) / span * (SPRITE_LEVELS - 1) + 0.5)
                   : 0;
            p->level = (short)(lv < 0 ? 0 : lv > SPRITE_LEVELS - 1 ? SPRITE_LEVELS - 1 : lv);
        }
        p->alpha = (float)frand_range(0.65, 1.0);
        p->ox = p->oy = 0.0f;
        p->sx = p->x;
        p->sy = p->y;
    }

    free(a->cell_of);   a->cell_of   = malloc(n * sizeof(int));
    free(a->order);     a->order     = malloc(n * sizeof(int));
    a->gw = a->gh = 0;                       /* force grid realloc */

    sprites_build(a);
}

static void particles_step(App *a, double dt)
{
    const Config *c = &a->cfg;
    const double rdist = c->repel_dist > 0 ? c->repel_dist : c->cursor_dist * 0.6;
    const double rd2   = rdist * rdist;
    const int repel    = a->cursor_on && (c->cursor_mode & CUR_REPEL);
    const int attract  = a->cursor_on && (c->cursor_mode & CUR_ATTRACT);
    /* frame-rate independent exponential approach */
    const double k = 1.0 - exp(-c->cursor_response * dt);
    int i;

    for (i = 0; i < a->n; i++) {
        Particle *p = &a->p[i];
        double tx = 0.0, ty = 0.0;

        p->x += (float)(p->vx * dt);
        p->y += (float)(p->vy * dt);

        if (c->edges == EDGE_WRAP) {
            double m = p->r * c->glow;
            if (p->x < -m)            p->x = (float)(a->w + m);
            else if (p->x > a->w + m) p->x = (float)(-m);
            if (p->y < -m)            p->y = (float)(a->h + m);
            else if (p->y > a->h + m) p->y = (float)(-m);
        } else {
            if (p->x < 0)         { p->x = 0;              p->vx = fabsf(p->vx);  }
            else if (p->x > a->w) { p->x = (float)a->w;    p->vx = -fabsf(p->vx); }
            if (p->y < 0)         { p->y = 0;              p->vy = fabsf(p->vy);  }
            else if (p->y > a->h) { p->y = (float)a->h;    p->vy = -fabsf(p->vy); }
        }

        /* The cursor does not accelerate particles - it bends the field. Each
         * particle springs toward a displaced target and relaxes back once the
         * cursor moves on, so a resting cursor holds a stable ring instead of
         * flinging everything off screen. */
        if (repel || attract) {
            double dx = p->x - a->cx, dy = p->y - a->cy;
            double d2 = dx * dx + dy * dy;
            if (d2 < rd2) {
                double d = sqrt(d2);
                if (d < 1e-3) { d = 1e-3; dx = 1e-3; dy = 0.0; }
                if (repel) {
                    double push = (rdist - d) * c->cursor_strength;
                    tx = dx / d * push;
                    ty = dy / d * push;
                } else {
                    double pull = c->cursor_strength * (1.0 - d / rdist);
                    if (pull > 0.85) pull = 0.85;
                    tx = -dx * pull;
                    ty = -dy * pull;
                }
            }
        }

        p->ox += (float)((tx - p->ox) * k);
        p->oy += (float)((ty - p->oy) * k);
        p->sx  = p->x + p->ox;
        p->sy  = p->y + p->oy;
    }
}

/* Bucket the particles into a uniform grid so the link pass only compares
 * neighbours instead of every pair. */
static void grid_build(App *a)
{
    int i, cells;
    double cell = a->cfg.link_dist > 8.0 ? a->cfg.link_dist : 8.0;
    int gw = (int)(a->w / cell) + 1;
    int gh = (int)(a->h / cell) + 1;

    if (gw != a->gw || gh != a->gh) {
        a->gw = gw; a->gh = gh;
        free(a->cell_start);  a->cell_start  = malloc((gw * gh + 1) * sizeof(int));
        free(a->cell_cursor); a->cell_cursor = malloc((gw * gh + 1) * sizeof(int));
    }
    a->cell = cell;
    cells = gw * gh;
    memset(a->cell_start, 0, (cells + 1) * sizeof(int));

    for (i = 0; i < a->n; i++) {
        int cx = (int)(a->p[i].sx / cell);
        int cy = (int)(a->p[i].sy / cell);
        if (cx < 0) cx = 0; else if (cx >= gw) cx = gw - 1;
        if (cy < 0) cy = 0; else if (cy >= gh) cy = gh - 1;
        a->cell_of[i] = cy * gw + cx;
        a->cell_start[a->cell_of[i] + 1]++;
    }
    for (i = 0; i < cells; i++) a->cell_start[i + 1] += a->cell_start[i];
    memcpy(a->cell_cursor, a->cell_start, (cells + 1) * sizeof(int));
    for (i = 0; i < a->n; i++) a->order[a->cell_cursor[a->cell_of[i]]++] = i;
}

/* ------------------------------------------------------------ damage bands */

static void damage_reset(App *a)
{
    int i;
    for (i = 0; i < a->bands; i++) { a->dmg_min[i] = INT32_MAX; a->dmg_max[i] = -1; }
}

static void damage_add(App *a, double x0, double y0, double x1, double y1)
{
    int ix0, ix1, b, b0, b1;

    if (x1 < 0 || y1 < 0 || x0 > a->w || y0 > a->h) return;
    ix0 = (int)floor(x0) - 1; if (ix0 < 0)     ix0 = 0;
    ix1 = (int)ceil (x1) + 1; if (ix1 > a->w - 1) ix1 = a->w - 1;
    if (ix1 < ix0) return;

    b0 = (int)floor(y0 - 1) / BAND_H; if (b0 < 0) b0 = 0;
    b1 = (int)ceil (y1 + 1) / BAND_H; if (b1 > a->bands - 1) b1 = a->bands - 1;

    for (b = b0; b <= b1; b++) {
        if (ix0 < a->dmg_min[b]) a->dmg_min[b] = ix0;
        if (ix1 > a->dmg_max[b]) a->dmg_max[b] = ix1;
    }
}

/* Erase what the previous frame drew: transparent when a compositor blends us
 * over the wallpaper, otherwise a copy of the wallpaper itself. */
static void damage_clear_previous(App *a)
{
    int b;
    for (b = 0; b < a->bands; b++) {
        int y, y0, y1, x0 = a->prev_min[b], x1 = a->prev_max[b];
        if (x1 < x0) continue;
        y0 = b * BAND_H;
        y1 = y0 + BAND_H; if (y1 > a->h) y1 = a->h;
        for (y = y0; y < y1; y++) {
            unsigned char *dst = a->pixels + (size_t)y * a->stride + (size_t)x0 * 4;
            size_t bytes = (size_t)(x1 - x0 + 1) * 4;
            if (a->bg) memcpy(dst, a->bg + (size_t)y * a->stride + (size_t)x0 * 4, bytes);
            else       memset(dst, 0, bytes);
        }
    }
}

/* Push the union of this frame's and last frame's damage to the server, merging
 * consecutive bands so a full-screen frame costs a single upload. */
static void present(App *a)
{
    int b = 0;
    while (b < a->bands) {
        int x0, x1, run_y0, run_y1;
        int lo = a->dmg_min[b] < a->prev_min[b] ? a->dmg_min[b] : a->prev_min[b];
        int hi = a->dmg_max[b] > a->prev_max[b] ? a->dmg_max[b] : a->prev_max[b];
        if (hi < lo) { b++; continue; }

        x0 = lo; x1 = hi;
        run_y0 = b * BAND_H;
        while (b < a->bands) {
            int nlo = a->dmg_min[b] < a->prev_min[b] ? a->dmg_min[b] : a->prev_min[b];
            int nhi = a->dmg_max[b] > a->prev_max[b] ? a->dmg_max[b] : a->prev_max[b];
            if (nhi < nlo) break;
            if (nlo < x0) x0 = nlo;
            if (nhi > x1) x1 = nhi;
            b++;
        }
        run_y1 = b * BAND_H; if (run_y1 > a->h) run_y1 = a->h;

        if (a->use_shm)
            XShmPutImage(a->dpy, a->win, a->gc, a->ximg, x0, run_y0, x0, run_y0,
                         x1 - x0 + 1, run_y1 - run_y0, False);
        else
            XPutImage(a->dpy, a->win, a->gc, a->ximg, x0, run_y0, x0, run_y0,
                      x1 - x0 + 1, run_y1 - run_y0);
    }

    { int *t;
      t = a->prev_min; a->prev_min = a->dmg_min; a->dmg_min = t;
      t = a->prev_max; a->prev_max = a->dmg_max; a->dmg_max = t; }
    damage_reset(a);
}

/* ----------------------------------------------------------------- drawing */

/* ------------------------------------------------------- software raster */
/*
 * Cairo is excellent but its fixed cost per stroke (~11us here) dwarfs the two
 * hundred pixels a link actually covers, and that cost is what pinned the old
 * build at ~30% of a core. These two routines cover everything this program
 * draws - antialiased segments and a pre-rendered dot - straight into the
 * premultiplied ARGB buffer. Coverage comes from the true distance to the
 * segment, so the edges stay as smooth as cairo's.
 */

/* one premultiplied OVER blend; cov is 0..255 */
static inline void blend_px(unsigned char *px, int R, int G, int B, int cov)
{
    unsigned ia;
    if (cov <= 0) return;
    if (cov > 255) cov = 255;
    ia = 255u - (unsigned)cov;
    px[0] = (unsigned char)((B * cov + px[0] * ia + 127) / 255);
    px[1] = (unsigned char)((G * cov + px[1] * ia + 127) / 255);
    px[2] = (unsigned char)((R * cov + px[2] * ia + 127) / 255);
    px[3] = (unsigned char)((255 * cov + px[3] * ia + 127) / 255);
}

static void draw_line_aa(App *a, double x0, double y0, double x1, double y1,
                         double hw, int R, int G, int B, double alpha)
{
    const double dx = x1 - x0, dy = y1 - y0;
    const double len2 = dx * dx + dy * dy;
    const double aa = hw + 0.5;            /* coverage reaches half a pixel out */
    const double inv_len2 = len2 > 1e-9 ? 1.0 / len2 : 0.0;
    unsigned char *base = a->pixels;
    int i, i0, i1;

    if (alpha <= 0.002) return;
    if (len2 <= 1e-9) return;

    if (fabs(dx) >= fabs(dy)) {            /* iterate columns */
        double slope = dy / dx;
        double ext = aa * sqrt(1.0 + slope * slope);
        i0 = (int)floor((x0 < x1 ? x0 : x1) - aa);
        i1 = (int)ceil ((x0 > x1 ? x0 : x1) + aa);
        if (i0 < 0) i0 = 0;
        if (i1 > a->w - 1) i1 = a->w - 1;
        for (i = i0; i <= i1; i++) {
            double px = i + 0.5;
            double t  = (px - x0) / dx;
            double yc, lo, hi;
            int j, j0, j1;
            if (t < 0) t = 0; else if (t > 1) t = 1;
            yc = y0 + t * dy;
            lo = yc - ext; hi = yc + ext;
            j0 = (int)floor(lo); j1 = (int)ceil(hi);
            if (j0 < 0) j0 = 0;
            if (j1 > a->h - 1) j1 = a->h - 1;
            for (j = j0; j <= j1; j++) {
                double vx = px - x0, vy = (j + 0.5) - y0;
                double s  = (vx * dx + vy * dy) * inv_len2;
                double ex, ey, cov;
                if (s < 0) s = 0; else if (s > 1) s = 1;
                ex = vx - s * dx; ey = vy - s * dy;
                cov = aa - sqrt(ex * ex + ey * ey);
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
                blend_px(base + (size_t)j * a->stride + (size_t)i * 4,
                         R, G, B, (int)(cov * alpha * 255.0 + 0.5));
            }
        }
    } else {                               /* iterate rows */
        double slope = dx / dy;
        double ext = aa * sqrt(1.0 + slope * slope);
        i0 = (int)floor((y0 < y1 ? y0 : y1) - aa);
        i1 = (int)ceil ((y0 > y1 ? y0 : y1) + aa);
        if (i0 < 0) i0 = 0;
        if (i1 > a->h - 1) i1 = a->h - 1;
        for (i = i0; i <= i1; i++) {
            double py = i + 0.5;
            double t  = (py - y0) / dy;
            double xc, lo, hi;
            int j, j0, j1;
            unsigned char *row;
            if (t < 0) t = 0; else if (t > 1) t = 1;
            xc = x0 + t * dx;
            lo = xc - ext; hi = xc + ext;
            j0 = (int)floor(lo); j1 = (int)ceil(hi);
            if (j0 < 0) j0 = 0;
            if (j1 > a->w - 1) j1 = a->w - 1;
            row = base + (size_t)i * a->stride;
            for (j = j0; j <= j1; j++) {
                double vx = (j + 0.5) - x0, vy = py - y0;
                double s  = (vx * dx + vy * dy) * inv_len2;
                double ex, ey, cov;
                if (s < 0) s = 0; else if (s > 1) s = 1;
                ex = vx - s * dx; ey = vy - s * dy;
                cov = aa - sqrt(ex * ex + ey * ey);
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
                blend_px(row + (size_t)j * 4, R, G, B,
                         (int)(cov * alpha * 255.0 + 0.5));
            }
        }
    }
}

/* Bilinear blit of an A8 dot sprite. The sample offset is constant across the
 * whole sprite, so the four filter weights are computed once. */
static void blit_sprite(App *a, int level, double x, double y,
                        int R, int G, int B, double alpha)
{
    const unsigned char *m = a->sprite[level];
    const int msz = a->sprite_size[level];
    const int mst = a->sprite_stride[level];
    double ux, uy, fu, fv, w00, w10, w01, w11;
    int bx, by, ix, iy, ix0, ix1, iy0, iy1;
    int ia = (int)(alpha * 255.0 + 0.5);

    if (!m || ia <= 0) return;
    if (ia > 255) ia = 255;

    ux = -x; uy = -y;
    bx = (int)floor(ux); by = (int)floor(uy);
    fu = ux - bx;        fv = uy - by;
    w00 = (1 - fu) * (1 - fv); w10 = fu * (1 - fv);
    w01 = (1 - fu) * fv;       w11 = fu * fv;

    ix0 = -bx - 1;  ix1 = -bx + msz;
    iy0 = -by - 1;  iy1 = -by + msz;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > a->w - 1) ix1 = a->w - 1;
    if (iy1 > a->h - 1) iy1 = a->h - 1;

    for (iy = iy0; iy <= iy1; iy++) {
        int sy = iy + by;
        const unsigned char *r0 = (sy     >= 0 && sy     < msz) ? m + (size_t)sy * mst : NULL;
        const unsigned char *r1 = (sy + 1 >= 0 && sy + 1 < msz) ? m + (size_t)(sy + 1) * mst : NULL;
        unsigned char *dst = a->pixels + (size_t)iy * a->stride + (size_t)ix0 * 4;
        if (!r0 && !r1) continue;
        for (ix = ix0; ix <= ix1; ix++, dst += 4) {
            int sx = ix + bx;
            double v = 0.0;
            int in0 = (sx     >= 0 && sx     < msz);
            int in1 = (sx + 1 >= 0 && sx + 1 < msz);
            if (r0) { if (in0) v += w00 * r0[sx]; if (in1) v += w10 * r0[sx + 1]; }
            if (r1) { if (in0) v += w01 * r1[sx]; if (in1) v += w11 * r1[sx + 1]; }
            if (v < 0.5) continue;
            blend_px(dst, R, G, B, (int)(v * ia / 255.0 + 0.5));
        }
    }
}

/* ----------------------------------------------------------------- drawing */

static void render(App *a)
{
    const Config *c = &a->cfg;
    const double lw = c->link_width, hw = lw * 0.5;
    const double D  = c->link_dist, D2 = D * D;
    const int lR = (int)(c->link_color[0] * 255 + 0.5);
    const int lG = (int)(c->link_color[1] * 255 + 0.5);
    const int lB = (int)(c->link_color[2] * 255 + 0.5);
    const int pR = (int)(c->color[0] * 255 + 0.5);
    const int pG = (int)(c->color[1] * 255 + 0.5);
    const int pB = (int)(c->color[2] * 255 + 0.5);
    int gx, gy, i;

    damage_clear_previous(a);

    /* links between neighbouring particles */
    grid_build(a);
    for (gy = 0; gy < a->gh; gy++) {
        for (gx = 0; gx < a->gw; gx++) {
            int cell = gy * a->gw + gx;
            int s = a->cell_start[cell], e = a->cell_start[cell + 1];
            int k;
            /* self + half the neighbourhood, so every pair is visited once */
            static const int nb[5][2] = { {0,0}, {1,0}, {-1,1}, {0,1}, {1,1} };
            for (k = 0; k < 5; k++) {
                int ox = gx + nb[k][0], oy = gy + nb[k][1];
                int os, oe, ii, jj;
                if (ox < 0 || ox >= a->gw || oy < 0 || oy >= a->gh) continue;
                os = a->cell_start[oy * a->gw + ox];
                oe = a->cell_start[oy * a->gw + ox + 1];
                for (ii = s; ii < e; ii++) {
                    const Particle *p = &a->p[a->order[ii]];
                    int start = (k == 0) ? ii + 1 : os;
                    for (jj = start; jj < oe; jj++) {
                        const Particle *q = &a->p[a->order[jj]];
                        double dx = q->sx - p->sx, dy = q->sy - p->sy;
                        double d2 = dx * dx + dy * dy, t, alpha;
                        if (d2 >= D2) continue;
                        t = 1.0 - sqrt(d2) / D;
                        alpha = t * (0.45 + 0.55 * t) * c->link_opacity;
                        if (alpha < 0.004) continue;
                        draw_line_aa(a, p->sx, p->sy, q->sx, q->sy, hw,
                                     lR, lG, lB, alpha);
                        damage_add(a,
                                   (p->sx < q->sx ? p->sx : q->sx) - lw,
                                   (p->sy < q->sy ? p->sy : q->sy) - lw,
                                   (p->sx > q->sx ? p->sx : q->sx) + lw,
                                   (p->sy > q->sy ? p->sy : q->sy) + lw);
                    }
                }
            }
        }
    }

    /* links from the cursor - the "grab" effect */
    if (a->cursor_on && (c->cursor_mode & CUR_GRAB)) {
        double CD = c->cursor_dist, CD2 = CD * CD;
        for (i = 0; i < a->n; i++) {
            const Particle *p = &a->p[i];
            double dx = p->sx - a->cx, dy = p->sy - a->cy;
            double d2 = dx * dx + dy * dy, t, alpha, w;
            if (d2 >= CD2) continue;
            t = 1.0 - sqrt(d2) / CD;
            alpha = t * (0.5 + 0.5 * t) * c->link_opacity * 1.7;
            if (alpha > 0.9) alpha = 0.9;
            if (alpha < 0.004) continue;
            w = hw * (0.85 + 0.5 * t);
            draw_line_aa(a, a->cx, a->cy, p->sx, p->sy, w, lR, lG, lB, alpha);
            damage_add(a,
                       (a->cx < p->sx ? a->cx : p->sx) - lw * 1.5,
                       (a->cy < p->sy ? a->cy : p->sy) - lw * 1.5,
                       (a->cx > p->sx ? a->cx : p->sx) + lw * 1.5,
                       (a->cy > p->sy ? a->cy : p->sy) + lw * 1.5);
        }
    }

    /* particles */
    for (i = 0; i < a->n; i++) {
        const Particle *p = &a->p[i];
        int lv = p->level;
        double off = a->sprite_c[lv];
        blit_sprite(a, lv, p->sx - off, p->sy - off, pR, pG, pB,
                    c->opacity * p->alpha);
        damage_add(a, p->sx - off - 1, p->sy - off - 1,
                      p->sx + off + 1, p->sy + off + 1);
    }
}

/* ---------------------------------------------------------- buffers and X11 */

static int compositor_running(Display *dpy, int screen)
{
    char buf[32];
    snprintf(buf, sizeof buf, "_NET_WM_CM_S%d", screen);
    return XGetSelectionOwner(dpy, XInternAtom(dpy, buf, False)) != None;
}

/* Copy the root wallpaper into a scratch buffer so we can restore from it while
 * running uncomposited. */
static void grab_wallpaper(App *a)
{
    Atom actual;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    Pixmap pm = None;
    Atom props[2];
    int i;

    if (!a->bg) return;
    memset(a->bg, 0, (size_t)a->stride * a->h);

    props[0] = XInternAtom(a->dpy, "_XROOTPMAP_ID", True);
    props[1] = XInternAtom(a->dpy, "ESETROOT_PMAP_ID", True);
    for (i = 0; i < 2 && pm == None; i++) {
        if (props[i] == None) continue;
        if (XGetWindowProperty(a->dpy, a->root, props[i], 0, 1, False, XA_PIXMAP,
                               &actual, &fmt, &nitems, &after, &data) == Success) {
            if (data && nitems == 1) pm = *(Pixmap *)data;
            if (data) { XFree(data); data = NULL; }
        }
    }
    if (pm == None) return;

    {
        Window r; int x, y; unsigned int pw, ph, bw, pd;
        cairo_surface_t *src, *dst;
        cairo_t *cr;
        if (!XGetGeometry(a->dpy, pm, &r, &x, &y, &pw, &ph, &bw, &pd)) return;
        src = cairo_xlib_surface_create(a->dpy, pm, DefaultVisual(a->dpy, a->screen),
                                        (int)pw, (int)ph);
        dst = cairo_image_surface_create_for_data(a->bg, CAIRO_FORMAT_RGB24,
                                                  a->w, a->h, a->stride);
        cr  = cairo_create(dst);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_flush(dst);
        cairo_surface_destroy(dst);
        cairo_surface_destroy(src);
    }
}

static void buffers_free(App *a)
{
    if (a->surface) { cairo_surface_destroy(a->surface); a->surface = NULL; }
    if (a->ximg) {
        if (a->use_shm) {
            XShmDetach(a->dpy, &a->shm);
            XDestroyImage(a->ximg);
            shmdt(a->shm.shmaddr);
        } else {
            XDestroyImage(a->ximg);     /* frees ximg->data too */
        }
        a->ximg = NULL;
    }
    free(a->bg);       a->bg       = NULL;
    free(a->dmg_min);  a->dmg_min  = NULL;
    free(a->dmg_max);  a->dmg_max  = NULL;
    free(a->prev_min); a->prev_min = NULL;
    free(a->prev_max); a->prev_max = NULL;
    a->pixels = NULL;
}

static int buffers_alloc(App *a)
{
    size_t size;
    cairo_format_t fmt = a->composited ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;

    a->use_shm = XShmQueryExtension(a->dpy);
    if (a->use_shm) {
        a->ximg = XShmCreateImage(a->dpy, a->visual, a->depth, ZPixmap, NULL,
                                  &a->shm, a->w, a->h);
        if (!a->ximg) a->use_shm = 0;
    }
    if (a->use_shm) {
        size = (size_t)a->ximg->bytes_per_line * a->ximg->height;
        a->shm.shmid = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);
        if (a->shm.shmid < 0) {
            XDestroyImage(a->ximg); a->ximg = NULL; a->use_shm = 0;
        } else {
            a->shm.shmaddr = a->ximg->data = shmat(a->shm.shmid, NULL, 0);
            a->shm.readOnly = False;
            if (a->shm.shmaddr == (char *)-1 || !XShmAttach(a->dpy, &a->shm)) {
                if (a->shm.shmaddr != (char *)-1) shmdt(a->shm.shmaddr);
                shmctl(a->shm.shmid, IPC_RMID, NULL);
                XDestroyImage(a->ximg); a->ximg = NULL; a->use_shm = 0;
            } else {
                XSync(a->dpy, False);
                shmctl(a->shm.shmid, IPC_RMID, NULL);   /* reclaimed on exit */
            }
        }
    }
    if (!a->ximg) {
        char *data = calloc(1, (size_t)a->w * a->h * 4);
        if (!data) return -1;
        a->ximg = XCreateImage(a->dpy, a->visual, a->depth, ZPixmap, 0, data,
                               a->w, a->h, 32, a->w * 4);
        if (!a->ximg) { free(data); return -1; }
    }

    a->pixels = (unsigned char *)a->ximg->data;
    a->stride = a->ximg->bytes_per_line;
    memset(a->pixels, 0, (size_t)a->stride * a->h);

    a->surface = cairo_image_surface_create_for_data(a->pixels, fmt, a->w, a->h, a->stride);
    if (cairo_surface_status(a->surface) != CAIRO_STATUS_SUCCESS) return -1;

    a->bands    = (a->h + BAND_H - 1) / BAND_H;
    a->dmg_min  = malloc(a->bands * sizeof(int));
    a->dmg_max  = malloc(a->bands * sizeof(int));
    a->prev_min = malloc(a->bands * sizeof(int));
    a->prev_max = malloc(a->bands * sizeof(int));
    damage_reset(a);
    { int i; for (i = 0; i < a->bands; i++) { a->prev_min[i] = INT32_MAX; a->prev_max[i] = -1; } }

    if (!a->composited) {
        a->bg = calloc(1, (size_t)a->stride * a->h);
        grab_wallpaper(a);
        if (a->bg) memcpy(a->pixels, a->bg, (size_t)a->stride * a->h);
    }
    return 0;
}

static void set_atom_prop(Display *d, Window w, const char *name,
                          const Atom *vals, int n)
{
    XChangeProperty(d, w, XInternAtom(d, name, False), XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)vals, n);
}

static int window_create(App *a)
{
    XVisualInfo vi;
    XSetWindowAttributes swa;
    unsigned long mask;
    Atom types[1], states[4];
    XClassHint ch;
    XserverRegion region;
    long pid = getpid();
    unsigned long all_desktops = 0xFFFFFFFFUL;

    a->composited = compositor_running(a->dpy, a->screen);

    if (a->composited && XMatchVisualInfo(a->dpy, a->screen, 32, TrueColor, &vi)) {
        a->visual = vi.visual;
        a->depth  = 32;
    } else {
        if (a->composited)
            fprintf(stderr, "%s: no 32-bit visual, falling back to wallpaper copy\n",
                    APP_NAME);
        a->composited = 0;
        a->visual = DefaultVisual(a->dpy, a->screen);
        a->depth  = DefaultDepth(a->dpy, a->screen);
    }

    a->cmap = XCreateColormap(a->dpy, a->root, a->visual, AllocNone);

    memset(&swa, 0, sizeof swa);
    swa.colormap          = a->cmap;
    swa.background_pixel  = 0;
    swa.border_pixel      = 0;
    swa.override_redirect = True;      /* the WM must not touch or stack this */
    swa.event_mask        = ExposureMask | StructureNotifyMask |
                            VisibilityChangeMask | PropertyChangeMask;
    mask = CWColormap | CWBackPixel | CWBorderPixel | CWOverrideRedirect | CWEventMask;

    a->win = XCreateWindow(a->dpy, a->root, 0, 0, a->w, a->h, 0, a->depth,
                           InputOutput, a->visual, mask, &swa);
    if (!a->win) return -1;

    types[0] = XInternAtom(a->dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    set_atom_prop(a->dpy, a->win, "_NET_WM_WINDOW_TYPE", types, 1);

    states[0] = XInternAtom(a->dpy, "_NET_WM_STATE_BELOW", False);
    states[1] = XInternAtom(a->dpy, "_NET_WM_STATE_STICKY", False);
    states[2] = XInternAtom(a->dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    states[3] = XInternAtom(a->dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    set_atom_prop(a->dpy, a->win, "_NET_WM_STATE", states, 4);

    XChangeProperty(a->dpy, a->win, XInternAtom(a->dpy, "_NET_WM_DESKTOP", False),
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&all_desktops, 1);
    XChangeProperty(a->dpy, a->win, XInternAtom(a->dpy, "_NET_WM_PID", False),
                    XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&pid, 1);

    ch.res_name  = (char *)"particles";
    ch.res_class = (char *)"Particles";
    XSetClassHint(a->dpy, a->win, &ch);
    XStoreName(a->dpy, a->win, "particles wallpaper");

    /* Empty input region: every click, scroll and key falls through to whatever
     * is below us. This is what makes the layer non-interactive. */
    region = XFixesCreateRegion(a->dpy, NULL, 0);
    XFixesSetWindowShapeRegion(a->dpy, a->win, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(a->dpy, region);

    a->atom_active     = XInternAtom(a->dpy, "_NET_ACTIVE_WINDOW", True);
    a->atom_state      = XInternAtom(a->dpy, "_NET_WM_STATE", False);
    a->atom_fullscreen = XInternAtom(a->dpy, "_NET_WM_STATE_FULLSCREEN", False);

    a->gc = XCreateGC(a->dpy, a->win, 0, NULL);

    XMapWindow(a->dpy, a->win);
    XLowerWindow(a->dpy, a->win);
    XFlush(a->dpy);
    return 0;
}

/* Is something covering the whole screen?
 *
 * VisibilityNotify cannot answer this: once a compositor redirects the windows
 * the server reports everything as unobscured, so the event never arrives and a
 * fullscreen game would leave us drawing frames nobody can see. Ask the window
 * manager instead - the active window is either flagged fullscreen or simply
 * big enough to hide us. Checking the compositor's own overlay window would be
 * ambiguous here, and the active window never is. */
static int screen_covered(App *a)
{
    Atom actual;
    int fmt, x, y;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    Window active = None, child;
    XWindowAttributes wa;
    int covered = 0;

    if (a->atom_active == None) return 0;
    if (XGetWindowProperty(a->dpy, a->root, a->atom_active, 0, 1, False, XA_WINDOW,
                           &actual, &fmt, &nitems, &after, &data) != Success)
        return 0;
    if (data) {
        if (nitems == 1) active = *(Window *)data;
        XFree(data);
    }
    if (active == None || active == a->win) return 0;

    data = NULL;
    if (XGetWindowProperty(a->dpy, active, a->atom_state, 0, 32, False, XA_ATOM,
                           &actual, &fmt, &nitems, &after, &data) == Success && data) {
        unsigned long i;
        Atom *states = (Atom *)data;
        for (i = 0; i < nitems; i++)
            if (states[i] == a->atom_fullscreen) covered = 1;
        XFree(data);
    }
    if (covered) return 1;

    if (!XGetWindowAttributes(a->dpy, active, &wa) || wa.map_state != IsViewable)
        return 0;
    if (!XTranslateCoordinates(a->dpy, active, a->root, 0, 0, &x, &y, &child))
        return 0;
    return x <= 0 && y <= 0 && x + wa.width >= a->w && y + wa.height >= a->h;
}

/* Some window managers restack on desktop switch; make sure we stay at the very
 * bottom without fighting anything that is already below us. */
static void keep_below(App *a)
{
    Window r, parent, *kids = NULL;
    unsigned int nkids = 0;
    if (!XQueryTree(a->dpy, a->root, &r, &parent, &kids, &nkids)) return;
    if (nkids > 0 && kids[0] != a->win) XLowerWindow(a->dpy, a->win);
    if (kids) XFree(kids);
}

static int resize(App *a, int w, int h)
{
    if (w == a->w && h == a->h) return 0;
    buffers_free(a);
    a->w = w; a->h = h;
    XMoveResizeWindow(a->dpy, a->win, 0, 0, w, h);
    if (buffers_alloc(a) < 0) return -1;
    particles_init(a);
    return 0;
}

static void damage_all_prev(App *a)
{
    int i;
    for (i = 0; i < a->bands; i++) { a->prev_min[i] = 0; a->prev_max[i] = a->w - 1; }
}

static void handle_events(App *a)
{
    while (XPending(a->dpy)) {
        XEvent ev;
        XNextEvent(a->dpy, &ev);

        if (a->has_randr && ev.type == a->rr_event_base + RRScreenChangeNotify) {
            XRRUpdateConfiguration(&ev);
            resize(a, DisplayWidth(a->dpy, a->screen), DisplayHeight(a->dpy, a->screen));
            continue;
        }

        switch (ev.type) {
        case Expose:
            damage_all_prev(a);
            break;
        case ConfigureNotify:
            if (ev.xconfigure.window == a->root)
                resize(a, ev.xconfigure.width, ev.xconfigure.height);
            else if (ev.xconfigure.window == a->win &&
                     (ev.xconfigure.width != a->w || ev.xconfigure.height != a->h))
                resize(a, ev.xconfigure.width, ev.xconfigure.height);
            break;
        case VisibilityNotify:
            a->obscured = (ev.xvisibility.state == VisibilityFullyObscured);
            if (!a->obscured) damage_all_prev(a);
            break;
        case PropertyNotify:
            /* wallpaper changed under us while running uncomposited */
            if (!a->composited && a->bg && ev.xproperty.window == a->root) {
                Atom p = ev.xproperty.atom;
                if (p == XInternAtom(a->dpy, "_XROOTPMAP_ID", True) ||
                    p == XInternAtom(a->dpy, "ESETROOT_PMAP_ID", True)) {
                    grab_wallpaper(a);
                    damage_all_prev(a);
                }
            }
            break;
        default:
            break;
        }
    }
}

static void poll_cursor(App *a, double dt)
{
    Window r, child;
    int rx, ry, wx, wy;
    unsigned int mods;
    double k;

    if (!XQueryPointer(a->dpy, a->root, &r, &child, &rx, &ry, &wx, &wy, &mods)) {
        a->cursor_on = 0;                 /* pointer is on another screen */
        return;
    }
    if (!a->cursor_on) { a->cx = rx; a->cy = ry; a->cursor_on = 1; }

    /* Ease toward the real position so fast cursor jumps do not snap the field. */
    k = 1.0 - exp(-a->cfg.cursor_ease * dt);
    a->cx += (rx - a->cx) * k;
    a->cy += (ry - a->cy) * k;
}

static void run(App *a)
{
    const double frame = 1.0 / (a->cfg.fps > 0 ? a->cfg.fps : 60);
    double last = now_sec();
    double next = last;
    double last_lower = last;
    double last_cover = 0;
    int    was_paused = 0;

    XSelectInput(a->dpy, a->root,
                 StructureNotifyMask | (a->composited ? 0 : PropertyChangeMask));
    if (a->has_randr) XRRSelectInput(a->dpy, a->root, RRScreenChangeNotifyMask);

    while (!g_quit) {
        double t, dt, interval;
        int paused;

        handle_events(a);

        if (g_reload) {
            char path[512];
            g_reload = 0;
            config_path(path, sizeof path);
            load_config_file(&a->cfg, path);
            particles_init(a);
            damage_all_prev(a);
        }

        t  = now_sec();
        dt = t - last;
        last = t;
        if (dt < 0)    dt = 0;
        if (dt > 0.1)  dt = 0.1;          /* survive suspend / long stalls */

        /* Nothing to draw while something covers the whole screen - a
         * fullscreen game, say - so idle instead of burning a core on it. */
        if (a->cfg.pause_when_covered && t - last_cover > 1.0) {
            a->covered = screen_covered(a);
            last_cover = t;
        }
        paused = a->cfg.pause_when_covered && (a->obscured || a->covered);
        interval = paused ? 0.1 : frame;

        if (was_paused && !paused) damage_all_prev(a);
        was_paused = paused;

        if (!paused) {
            poll_cursor(a, dt);
            particles_step(a, dt);
            render(a);
            present(a);
            XSync(a->dpy, False);          /* pace with the server, no frame pileup */
        } else {
            particles_step(a, dt);
        }

        if (a->cfg.keep_below && t - last_lower > 2.0) {
            keep_below(a);
            last_lower = t;
        }

        next += interval;
        if (next < t) next = t + interval; /* we fell behind; do not spiral */
        {
            double sleep_for = next - now_sec();
            if (sleep_for > 0) {
                struct timespec ts;
                ts.tv_sec  = (time_t)sleep_for;
                ts.tv_nsec = (long)((sleep_for - ts.tv_sec) * 1e9);
                nanosleep(&ts, NULL);
            }
        }
    }
}

/* ------------------------------------------------- single instance / toggle */

static void lock_path(char *buf, size_t n)
{
    const char *rt  = getenv("XDG_RUNTIME_DIR");
    const char *dpy = getenv("DISPLAY");
    char safe[64], *p;
    snprintf(safe, sizeof safe, "%s", dpy && *dpy ? dpy : "0");
    for (p = safe; *p; p++) if (*p == '/' || *p == ':') *p = '_';
    if (rt && *rt) snprintf(buf, n, "%s/particles-%s.lock", rt, safe);
    else           snprintf(buf, n, "/tmp/particles-%u-%s.lock",
                            (unsigned)getuid(), safe);
}

/* Returns the fd holding the lock, or -1 when another instance owns it (with
 * *other set to its pid). */
static int lock_acquire(pid_t *other)
{
    char path[512];
    int fd;
    lock_path(path, sizeof path);
    fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        fprintf(stderr, "%s: cannot open %s: %s\n", APP_NAME, path, strerror(errno));
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;

    if (other) {
        char buf[32] = {0};
        lseek(fd, 0, SEEK_SET);
        if (read(fd, buf, sizeof buf - 1) > 0) *other = (pid_t)atoi(buf);
        else *other = 0;
    }
    close(fd);
    return -1;
}

static void lock_write_pid(int fd)
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d\n", (int)getpid());
    if (ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0) {
        ssize_t w = write(fd, buf, n);
        (void)w;
    }
}

static int stop_running(pid_t pid)
{
    int i;
    if (pid <= 0) return -1;
    if (kill(pid, SIGTERM) != 0) return -1;
    for (i = 0; i < 200; i++) {                 /* wait up to ~2s for a clean exit */
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        if (kill(pid, 0) != 0) return 0;
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    return 0;
}

static int daemonize(const char *log_path)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);                      /* the X fd lives on in the child */
    if (setsid() < 0) return -1;
    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    {
        int null = open("/dev/null", O_RDWR);
        int out  = log_path && *log_path
                 ? open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
        if (null >= 0) dup2(null, STDIN_FILENO);
        dup2(out >= 0 ? out : (null >= 0 ? null : STDERR_FILENO), STDOUT_FILENO);
        dup2(out >= 0 ? out : (null >= 0 ? null : STDERR_FILENO), STDERR_FILENO);
        if (null > 2) close(null);
        if (out  > 2) close(out);
    }
    return 0;
}

/* ---------------------------------------------------------------- snapshot */

/* Render a single settled frame to a PNG without touching the screen, so a
 * config can be previewed before it goes live. */
static int snapshot(Config *cfg, const char *path, int w, int h, const char *bg)
{
    App a;
    int i, rc = 0;
    cairo_surface_t *out;
    cairo_t *cr;

    memset(&a, 0, sizeof a);
    a.cfg = *cfg;
    a.w = w; a.h = h;
    a.stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, w);
    a.pixels = calloc(1, (size_t)a.stride * h);
    if (!a.pixels) return -1;

    a.surface = cairo_image_surface_create_for_data(a.pixels, CAIRO_FORMAT_ARGB32,
                                                    w, h, a.stride);
    a.bands = (h + BAND_H - 1) / BAND_H;
    a.dmg_min  = malloc(a.bands * sizeof(int));
    a.dmg_max  = malloc(a.bands * sizeof(int));
    a.prev_min = malloc(a.bands * sizeof(int));
    a.prev_max = malloc(a.bands * sizeof(int));
    damage_reset(&a);
    for (i = 0; i < a.bands; i++) { a.prev_min[i] = INT32_MAX; a.prev_max[i] = -1; }

    srand(1234);
    particles_init(&a);
    a.cx = w / 2.0; a.cy = h / 2.0; a.cursor_on = 1;
    for (i = 0; i < 120; i++) particles_step(&a, 1.0 / 60.0);   /* let it settle */
    render(&a);
    cairo_surface_mark_dirty(a.surface);

    /* flatten over a backdrop - by default a dark ground, or the wallpaper
     * itself, so the colours can be judged where they will actually live */
    out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cr  = cairo_create(out);
    {
        double col[3] = { 0.055, 0.06, 0.075 };
        cairo_surface_t *img = NULL;
        if (bg && *bg == '#') parse_color(bg, col);
        else if (bg) {
            img = cairo_image_surface_create_from_png(bg);
            if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
                cairo_surface_destroy(img);
                img = NULL;
                fprintf(stderr, "%s: cannot read %s as PNG, using a flat backdrop\n",
                        APP_NAME, bg);
            }
        }
        if (img) {
            int iw = cairo_image_surface_get_width(img);
            int ih = cairo_image_surface_get_height(img);
            double sc = (double)w / iw;
            if ((double)h / ih > sc) sc = (double)h / ih;
            cairo_save(cr);
            cairo_scale(cr, sc, sc);
            cairo_set_source_surface(cr, img, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_surface_destroy(img);
        } else {
            cairo_set_source_rgb(cr, col[0], col[1], col[2]);
            cairo_paint(cr);
        }
    }
    cairo_set_source_surface(cr, a.surface, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    if (cairo_surface_write_to_png(out, path) != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "%s: cannot write %s\n", APP_NAME, path);
        rc = -1;
    } else {
        printf("%s: wrote %s (%dx%d, %d particles)\n", APP_NAME, path, w, h, a.n);
    }
    cairo_surface_destroy(out);

    cairo_surface_destroy(a.surface);
    free(a.pixels); free(a.p); free(a.cell_of); free(a.cell_start);
    free(a.cell_cursor); free(a.order);
    sprites_free(&a);
    free(a.dmg_min); free(a.dmg_max); free(a.prev_min); free(a.prev_max);
    return rc;
}

/* -------------------------------------------------------------------- main */

static void usage(FILE *out)
{
    fprintf(out,
"%s %s - antialiased particle field on the X11 wallpaper layer\n"
"\n"
"Usage: %s [action] [options]\n"
"\n"
"Actions (default: toggle)\n"
"  --toggle              start if not running, stop if it is\n"
"  --start               start only (no-op if already running)\n"
"  --stop                stop a running instance\n"
"  --restart             stop then start, picking up new options\n"
"  --status              report whether an instance is running\n"
"  -f, --foreground      run in this terminal instead of detaching\n"
"  --config FILE         read options from FILE\n"
"  --snapshot FILE       render one frame to a PNG and exit (preview)\n"
"  --snapshot-size WxH   size for --snapshot                [screen size]\n"
"  --snapshot-bg X       backdrop for --snapshot: a PNG path or #RRGGBB\n"
"  --no-config           ignore ~/.config/particles.conf\n"
"  -h, --help            this text\n"
"  -V, --version         version\n"
"\n"
"Options (also valid as 'key = value' lines in ~/.config/particles.conf)\n"
"  --count N             fixed particle count (0 = derive from --density)\n"
"  --density N           particles per megapixel            [70]\n"
"  --radius N            particle radius, spread around N   [1.8]\n"
"  --radius-min N        minimum radius                     [1.1]\n"
"  --radius-max N        maximum radius                     [2.6]\n"
"  --speed N             drift speed in px/s                [26]\n"
"  --link-distance N     px at which particles connect      [130]\n"
"  --link-width N        connecting line width              [1.1]\n"
"  --link-opacity N      0..1                               [0.55]\n"
"  --opacity N           particle opacity, 0..1             [0.85]\n"
"  --glow N              halo radius as a multiple of the dot [3.2]\n"
"  --color RRGGBB        particle colour                    [dbe8ff]\n"
"  --link-color RRGGBB   line colour                        [aed1ff]\n"
"  --cursor MODE         none|grab|repel|attract, combine with ','  [grab,repel]\n"
"  --cursor-distance N   radius the cursor links reach        [170]\n"
"  --repel-distance N    radius the cursor pushes within      [0.6x cursor-distance]\n"
"  --cursor-strength N   1.0 pushes particles onto the rim   [1.0]\n"
"  --cursor-response N   how fast the push settles, 1/s     [9]\n"
"  --edges MODE          bounce|wrap                        [bounce]\n"
"  --fps N               frame rate cap                     [60]\n"
"  --keep-below BOOL     re-lower the window periodically   [true]\n"
"  --pause-when-covered BOOL  skip drawing while fully hidden [true]\n"
"  --log FILE            where the detached process writes messages\n"
"\n"
"Signals: SIGHUP re-reads the config file, SIGTERM/SIGINT exit cleanly.\n",
    APP_NAME, APP_VERSION, APP_NAME);
}

enum { ACT_TOGGLE, ACT_START, ACT_STOP, ACT_RESTART, ACT_STATUS };

int main(int argc, char **argv)
{
    App app;
    Config cli;
    Config cfg;
    int action = ACT_TOGGLE;
    int foreground = 0, use_config = 1;
    const char *config_file = NULL;
    const char *snapshot_file = NULL;
    const char *snapshot_bg = NULL;
    int snap_w = 0, snap_h = 0;
    char cfgbuf[512];
    int lock_fd;
    pid_t other = 0;
    int i;
    /* Options given on the command line must win over the config file, so they
     * are collected separately and replayed after the file is read. */
    const char *ckey[64];
    const char *cval[64];
    int ncli = 0;

    config_defaults(&cli);

    for (i = 1; i < argc; i++) {
        char *arg = argv[i];
        char keybuf[64];
        const char *key, *val = NULL;
        char *eq;

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help"))    { usage(stdout); return 0; }
        if (!strcmp(arg, "-V") || !strcmp(arg, "--version")) {
            printf("%s %s\n", APP_NAME, APP_VERSION); return 0;
        }
        if (!strcmp(arg, "-f") || !strcmp(arg, "--foreground")) { foreground = 1; continue; }
        if (!strcmp(arg, "--toggle"))  { action = ACT_TOGGLE;  continue; }
        if (!strcmp(arg, "--start"))   { action = ACT_START;   continue; }
        if (!strcmp(arg, "--stop"))    { action = ACT_STOP;    continue; }
        if (!strcmp(arg, "--restart")) { action = ACT_RESTART; continue; }
        if (!strcmp(arg, "--status"))  { action = ACT_STATUS;  continue; }
        if (!strcmp(arg, "--no-config")) { use_config = 0; continue; }
        if (!strcmp(arg, "--snapshot")) {
            if (++i >= argc) { fprintf(stderr, "%s: --snapshot needs a path\n", APP_NAME); return 2; }
            snapshot_file = argv[i];
            continue;
        }
        if (!strcmp(arg, "--snapshot-bg")) {
            if (++i >= argc) { fprintf(stderr, "%s: --snapshot-bg needs a PNG or #RRGGBB\n", APP_NAME); return 2; }
            snapshot_bg = argv[i];
            continue;
        }
        if (!strcmp(arg, "--snapshot-size")) {
            if (++i >= argc || sscanf(argv[i], "%dx%d", &snap_w, &snap_h) != 2) {
                fprintf(stderr, "%s: --snapshot-size wants WxH\n", APP_NAME);
                return 2;
            }
            continue;
        }
        if (!strcmp(arg, "--config")) {
            if (++i >= argc) { fprintf(stderr, "%s: --config needs a path\n", APP_NAME); return 2; }
            config_file = argv[i];
            continue;
        }
        if (strncmp(arg, "--", 2) != 0) {
            fprintf(stderr, "%s: unexpected argument '%s'\n", APP_NAME, arg);
            usage(stderr);
            return 2;
        }

        snprintf(keybuf, sizeof keybuf, "%s", arg + 2);
        if ((eq = strchr(keybuf, '='))) { *eq = '\0'; val = arg + 2 + (eq - keybuf) + 1; }
        else {
            if (++i >= argc) {
                fprintf(stderr, "%s: option --%s needs a value\n", APP_NAME, keybuf);
                return 2;
            }
            val = argv[i];
        }
        key = keybuf;
        {
            int rc = set_option(&cli, key, val);
            if (rc == -2) { fprintf(stderr, "%s: unknown option --%s\n", APP_NAME, key); return 2; }
            if (rc != 0)  { fprintf(stderr, "%s: bad value for --%s: %s\n", APP_NAME, key, val); return 2; }
        }
        if (ncli < (int)(sizeof ckey / sizeof *ckey)) {
            ckey[ncli] = strdup(key);
            cval[ncli] = val;
            ncli++;
        }
    }

    if (snapshot_file) {
        config_defaults(&cfg);
        if (use_config) {
            if (config_file) load_config_file(&cfg, config_file);
            else { config_path(cfgbuf, sizeof cfgbuf); load_config_file(&cfg, cfgbuf); }
        }
        for (i = 0; i < ncli; i++) set_option(&cfg, ckey[i], cval[i]);
        if (snap_w <= 0 || snap_h <= 0) {
            Display *d = XOpenDisplay(NULL);
            if (d) {
                snap_w = DisplayWidth(d, DefaultScreen(d));
                snap_h = DisplayHeight(d, DefaultScreen(d));
                XCloseDisplay(d);
            } else { snap_w = 1920; snap_h = 1080; }
        }
        return snapshot(&cfg, snapshot_file, snap_w, snap_h, snapshot_bg) == 0 ? 0 : 1;
    }

    /* action dispatch ----------------------------------------------------- */
    lock_fd = lock_acquire(&other);

    if (action == ACT_STATUS) {
        if (lock_fd < 0) { printf("running (pid %d)\n", (int)other); return 0; }
        close(lock_fd);
        printf("not running\n");
        return 1;
    }
    if (action == ACT_STOP) {
        if (lock_fd < 0) {
            if (stop_running(other) == 0) { printf("%s: stopped (pid %d)\n", APP_NAME, (int)other); return 0; }
            fprintf(stderr, "%s: could not stop pid %d\n", APP_NAME, (int)other);
            return 1;
        }
        close(lock_fd);
        fprintf(stderr, "%s: not running\n", APP_NAME);
        return 1;
    }
    if (lock_fd < 0) {
        /* somebody else holds the lock */
        if (action == ACT_START) {
            fprintf(stderr, "%s: already running (pid %d)\n", APP_NAME, (int)other);
            return 1;
        }
        if (stop_running(other) != 0) {
            fprintf(stderr, "%s: could not stop pid %d\n", APP_NAME, (int)other);
            return 1;
        }
        if (action == ACT_TOGGLE) { printf("%s: stopped\n", APP_NAME); return 0; }
        /* ACT_RESTART: take the lock now that the old one is gone */
        for (i = 0; i < 100 && (lock_fd = lock_acquire(&other)) < 0; i++) {
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (lock_fd < 0) { fprintf(stderr, "%s: lock still held\n", APP_NAME); return 1; }
    }

    /* config: defaults <- file <- command line ---------------------------- */
    config_defaults(&cfg);
    if (use_config) {
        if (config_file) load_config_file(&cfg, config_file);
        else { config_path(cfgbuf, sizeof cfgbuf); load_config_file(&cfg, cfgbuf); }
    }
    for (i = 0; i < ncli; i++) set_option(&cfg, ckey[i], cval[i]);

    if (cfg.radius_min > cfg.radius_max) {
        double t = cfg.radius_min; cfg.radius_min = cfg.radius_max; cfg.radius_max = t;
    }
    if (cfg.radius_min < 0.2) cfg.radius_min = 0.2;
    if (cfg.fps < 1)   cfg.fps = 1;
    if (cfg.fps > 240) cfg.fps = 240;
    if (cfg.glow < 1.0) cfg.glow = 1.0;

    /* bring it up --------------------------------------------------------- */
    memset(&app, 0, sizeof app);
    app.cfg = cfg;

    app.dpy = XOpenDisplay(NULL);
    if (!app.dpy) {
        fprintf(stderr, "%s: cannot open display '%s'\n", APP_NAME,
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    app.screen = DefaultScreen(app.dpy);
    app.root   = RootWindow(app.dpy, app.screen);
    app.w      = DisplayWidth(app.dpy, app.screen);
    app.h      = DisplayHeight(app.dpy, app.screen);

    {
        int fixes_event, fixes_error;
        if (!XFixesQueryExtension(app.dpy, &fixes_event, &fixes_error)) {
            fprintf(stderr, "%s: XFixes is required for click-through\n", APP_NAME);
            return 1;
        }
    }
    app.has_randr = XRRQueryExtension(app.dpy, &app.rr_event_base, &app.rr_error_base);

    if (window_create(&app) < 0) {
        fprintf(stderr, "%s: cannot create the overlay window\n", APP_NAME);
        return 1;
    }
    if (buffers_alloc(&app) < 0) {
        fprintf(stderr, "%s: cannot allocate the frame buffer\n", APP_NAME);
        return 1;
    }

    srand((unsigned)(time(NULL) ^ getpid()));
    particles_init(&app);

    fprintf(stdout, "%s: %dx%d, %d particles, %s, %d fps%s\n",
            APP_NAME, app.w, app.h, app.n,
            app.composited ? "ARGB via compositor" : "wallpaper copy (no compositor)",
            app.cfg.fps, app.use_shm ? ", MIT-SHM" : "");
    fflush(stdout);

    if (!foreground && daemonize(app.cfg.log_path) < 0) {
        fprintf(stderr, "%s: cannot detach: %s\n", APP_NAME, strerror(errno));
        return 1;
    }
    lock_write_pid(lock_fd);

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_signal;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGHUP,  &sa, NULL);
        signal(SIGPIPE, SIG_IGN);
    }

    run(&app);

    buffers_free(&app);
    if (app.gc) XFreeGC(app.dpy, app.gc);
    XDestroyWindow(app.dpy, app.win);
    XFreeColormap(app.dpy, app.cmap);
    XCloseDisplay(app.dpy);
    free(app.p);
    free(app.cell_of);
    free(app.cell_start);
    free(app.cell_cursor);
    free(app.order);
    sprites_free(&app);
    if (lock_fd >= 0) close(lock_fd);
    return 0;
}
