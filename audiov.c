#define _GNU_SOURCE   /* expose locale_t, M_PI, usleep on glibc/musl */

/*
 * audiov — Desktop Audio Visualizer for X11 + PipeWire
 *
 * Renders a spectrum-bar visualizer at the bottom of the screen,
 * behind all other windows (like conky). Captures audio from the
 * default PipeWire sink monitor (whatever is currently playing).
 *
 * Compile:
 *   make
 * or manually:
 *   gcc -O2 -o audiov audiov.c \
 *       $(pkg-config --libs --cflags libpipewire-0.3 x11) \
 *       -lm -lpthread
 *
 * Dependencies:
 *   Ubuntu/Debian: sudo apt install libpipewire-0.3-dev libx11-dev
 *   Arch Linux:    sudo pacman -S pipewire libx11
 *   Fedora:        sudo dnf install pipewire-devel libX11-devel
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/extensions/shape.h>

#include <gamemode_client.h>  /* header-only, no extra link flags */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

/* ═══════════════════════════════════════════════════════════════════
 * Configuration — edit these to your taste
 * ═══════════════════════════════════════════════════════════════════ */

#define CFG_FFT_SIZE      4096    /* FFT window size — 4096 gives ~10.7 Hz/bin at 44100, enough unique bins for all bass bars */
#define CFG_NUM_BARS      100     /* number of frequency bars              */
#define CFG_WIN_HEIGHT    200     /* visualizer window height in pixels    */
#define CFG_MARGIN_X      0      /* left/right screen margin (0=edge)     */
#define CFG_MARGIN_Y      0      /* gap from bottom of screen (0=flush)   */
#define CFG_BAR_GAP       1      /* pixel gap between bars                */
#define CFG_GRAVITY       0.0018f /* fall acceleration per frame          */
#define CFG_RISE_SPEED    0.85f  /* rise smoothing (higher = snappier)   */
#define CFG_SCALE         220.0f /* amplitude scale (raise if too quiet)  */
#define CFG_SAMPLE_RATE   44100  /* audio sample rate                     */
#define CFG_FPS           60     /* render framerate                      */
#define CFG_GAMEMODE_CHECK_SECS 2 /* how often to poll gamemode status    */
#define CFG_ON_TOP          0    /* 1=always on top, 0=stay behind       */
#define CFG_IDLE_SECS     3      /* silence timeout before hiding (secs)  */
#define CFG_SILENCE_RMS   0.0004f/* RMS threshold below which = silence   */

/* Background — fully transparent (compositor does the blending) */
#define COL_BG_A   0x00
#define COL_BG_R   0x00
#define COL_BG_G   0x00
#define COL_BG_B   0x00

/* Bar colour — single solid cyan, like cava's default */
#define COL_BAR_A  0xb3  /* ~70% opaque */
#define COL_BAR_R  0xff
#define COL_BAR_G  0x00
#define COL_BAR_B  0x00

/* Runtime colour values — defaults come from the defines above,
 * overridden by Xresources at startup (see load_xresources). */
static int g_xr_bar_r = COL_BAR_R, g_xr_bar_g = COL_BAR_G,
           g_xr_bar_b = COL_BAR_B, g_xr_bar_a = COL_BAR_A;
static int g_xr_bg_r  = COL_BG_R,  g_xr_bg_g  = COL_BG_G,
           g_xr_bg_b  = COL_BG_B,  g_xr_bg_a  = COL_BG_A;

/* ═══════════════════════════════════════════════════════════════════
 * Global state
 * ═══════════════════════════════════════════════════════════════════ */

/* Audio ring buffer */
static float            g_pcm[CFG_FFT_SIZE];
static volatile int     g_pcm_pos = 0;
static pthread_mutex_t  g_mutex   = PTHREAD_MUTEX_INITIALIZER;

/* Visualizer bars + per-bar fall velocity */
static float g_bars[CFG_NUM_BARS];
static float g_bar_vel[CFG_NUM_BARS];

/* Control */
static volatile int g_running = 1;
static int          g_visible     = 1;    /* is the window currently mapped?  */
static int          g_gamemode    = 0;    /* gamemode currently active?       */
static int          g_on_top      = CFG_ON_TOP; /* always-on-top mode        */
static int          g_silence_frames = 0; /* consecutive silent frames so far */

/* PipeWire */
static struct pw_main_loop *g_pw_loop;
static struct pw_stream    *g_pw_stream;
static struct spa_hook      g_stream_hook;
static struct pw_context   *g_pw_ctx;
static struct pw_core      *g_pw_core;

/* X11 */
static Display      *g_dpy;
static Window        g_win;
static GC            g_gc;
static int           g_screen;
static int           g_sw, g_sh;           /* screen size  */
static int           g_ww, g_wh;           /* window size  */
static Visual       *g_visual;             /* 32-bit ARGB visual */
static Colormap      g_cmap;
static Pixmap        g_pixmap;             /* off-screen back buffer */
static unsigned long g_col_bar;   /* single bar colour */
static unsigned long g_col_bg;

/* ═══════════════════════════════════════════════════════════════════
 * FFT — iterative Cooley-Tukey with Hann window
 * ═══════════════════════════════════════════════════════════════════ */

static void fft_forward(const float *in, float *mag_out, int n)
{
    /* Static buffer — avoids 60 malloc/free calls per second */
    static float complex buf[CFG_FFT_SIZE];
    if (n > CFG_FFT_SIZE) return;

    /* Apply Hann window */
    for (int i = 0; i < n; i++)
        buf[i] = in[i] * (0.5f - 0.5f * cosf(2.0f * M_PI * i / (n - 1)));

    /* Bit-reversal permutation */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float complex tmp = buf[i];
            buf[i] = buf[j];
            buf[j] = tmp;
        }
    }

    /* Butterfly passes */
    for (int len = 2; len <= n; len <<= 1) {
        float ang   = -2.0f * M_PI / (float)len;
        float complex wlen = cosf(ang) + I * sinf(ang);
        for (int i = 0; i < n; i += len) {
            float complex w = 1.0f + 0.0f * I;
            for (int k = 0; k < len / 2; k++) {
                float complex u = buf[i + k];
                float complex v = buf[i + k + len/2] * w;
                buf[i + k]         = u + v;
                buf[i + k + len/2] = u - v;
                w *= wlen;
            }
        }
    }

    for (int i = 0; i < n / 2; i++)
        mag_out[i] = cabsf(buf[i]) / (float)n;

}


/* Map magnitude bins → bars on a logarithmic frequency axis.
 *
 * Problem: at the low end, many bars map to the same 1-2 FFT bins
 * (e.g. bins 0-1 cover 0-43 Hz, but bar slots span 20-30 Hz, 30-45 Hz…).
 * Those bars get identical values → flat plateau in the centre.
 *
 * Fix (same approach as CAVA):
 *   1. Compute the ideal log-spaced bin boundary for every bar.
 *   2. Do a forward pass ensuring each bar's bin_lo is strictly greater
 *      than the previous bar's bin_lo, consuming one new bin per bar
 *      when the log mapping would otherwise repeat.
 *   3. Any bars that still share a range are collapsed: they inherit the
 *      value of the first bar in the group (interpolated later by gravity).
 *
 * Physics: bars rise fast (smoothed), fall with gravity acceleration.
 */
static void update_bars(const float *mag, int nbins)
{
    const float f_lo   = logf(50.0f);   /* 50 Hz floor — clean bass start */
    const float f_hi   = logf(18000.0f);
    const float bin_hz = (float)CFG_SAMPLE_RATE / (float)CFG_FFT_SIZE;

    /* ── Pass 1: compute raw log-spaced bin boundaries ── */
    int bin_lo[CFG_NUM_BARS], bin_hi[CFG_NUM_BARS];
    for (int b = 0; b < CFG_NUM_BARS; b++) {
        float freq_lo = expf(f_lo + (f_hi - f_lo) *  b      / CFG_NUM_BARS);
        float freq_hi = expf(f_lo + (f_hi - f_lo) * (b + 1) / CFG_NUM_BARS);
        bin_lo[b] = (int)(freq_lo / bin_hz);
        bin_hi[b] = (int)(freq_hi / bin_hz) + 1;
        /* Skip bins 0 and 1 — DC and near-DC are always garbage */
        if (bin_lo[b] < 2)  bin_lo[b] = 2;
        if (bin_hi[b] < 3)  bin_hi[b] = 3;
        if (bin_lo[b] >= nbins) bin_lo[b] = nbins - 1;
        if (bin_hi[b] >  nbins) bin_hi[b] = nbins;
        if (bin_hi[b] <= bin_lo[b]) bin_hi[b] = bin_lo[b] + 1;
    }

    /* ── Pass 2: enforce strictly increasing bin_lo ──────────────────
     * When the log mapping gives the same bin to several consecutive
     * bars, nudge each one forward by one bin so every bar is unique. */
    for (int b = 1; b < CFG_NUM_BARS; b++) {
        if (bin_lo[b] <= bin_lo[b-1]) {
            bin_lo[b] = bin_lo[b-1] + 1;
            if (bin_lo[b] >= nbins) bin_lo[b] = nbins - 1;
        }
        if (bin_hi[b] <= bin_lo[b]) bin_hi[b] = bin_lo[b] + 1;
        if (bin_hi[b] >  nbins)     bin_hi[b] = nbins;
    }

    /* ── Pass 3: compute magnitudes and apply physics ── */
    for (int b = 0; b < CFG_NUM_BARS; b++) {
        /* Use peak rather than average — averaging wide high-freq bin ranges
         * dilutes the signal when only one bin has energy (e.g. a sine sweep).
         * Peak picks up the loudest bin regardless of range width. */
        float peak = 0.0f;
        for (int i = bin_lo[b]; i < bin_hi[b]; i++)
            if (mag[i] > peak) peak = mag[i];

        /* Gentle treble boost: high-freq bars get progressively more gain
         * to compensate for natural 1/f rolloff and perceptual weighting.
         * t=0 at bass end, t=1 at treble end. Boost up to ~3x at the top. */
        float t      = (float)b / (float)(CFG_NUM_BARS - 1);
        float boost  = 1.0f + t * t * 2.0f;
        float target = peak * CFG_SCALE * boost;
        if (target > 1.0f) target = 1.0f;

        if (target >= g_bars[b]) {
            g_bars[b]    = g_bars[b] * (1.0f - CFG_RISE_SPEED) + target * CFG_RISE_SPEED;
            g_bar_vel[b] = 0.0f;
        } else {
            g_bar_vel[b] += CFG_GRAVITY;
            g_bars[b]    -= g_bar_vel[b];
            if (g_bars[b] < 0.0f) {
                g_bars[b]    = 0.0f;
                g_bar_vel[b] = 0.0f;
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * PipeWire audio capture
 * ═══════════════════════════════════════════════════════════════════ */

static void pw_on_process(void *userdata)
{
    (void)userdata;
    struct pw_buffer *pwb = pw_stream_dequeue_buffer(g_pw_stream);
    if (!pwb) return;

    struct spa_buffer *buf = pwb->buffer;
    float *data = buf->datas[0].data;
    if (!data) goto done;

    uint32_t n_frames = buf->datas[0].chunk->size / sizeof(float);

    pthread_mutex_lock(&g_mutex);
    for (uint32_t i = 0; i < n_frames; i++) {
        g_pcm[g_pcm_pos % CFG_FFT_SIZE] = data[i];
        g_pcm_pos++;
    }
    pthread_mutex_unlock(&g_mutex);

done:
    pw_stream_queue_buffer(g_pw_stream, pwb);
}

static const struct pw_stream_events pw_stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = pw_on_process,
};

static void *pw_thread_fn(void *arg)
{
    (void)arg;
    pw_main_loop_run(g_pw_loop);
    return NULL;
}

static int setup_pipewire(void)
{
    pw_init(NULL, NULL);

    g_pw_loop = pw_main_loop_new(NULL);
    if (!g_pw_loop) {
        fprintf(stderr, "audiov: pw_main_loop_new failed\n");
        return -1;
    }

    g_pw_ctx = pw_context_new(pw_main_loop_get_loop(g_pw_loop), NULL, 0);
    if (!g_pw_ctx) {
        fprintf(stderr, "audiov: pw_context_new failed\n");
        return -1;
    }

    g_pw_core = pw_context_connect(g_pw_ctx, NULL, 0);
    if (!g_pw_core) {
        fprintf(stderr, "audiov: pw_context_connect failed — is PipeWire running?\n");
        return -1;
    }

    g_pw_stream = pw_stream_new(
        g_pw_core, "audiov",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE,           "Audio",
            PW_KEY_MEDIA_CATEGORY,       "Capture",
            PW_KEY_MEDIA_ROLE,           "Music",
            PW_KEY_STREAM_CAPTURE_SINK,  "true",   /* capture monitor/loopback */
            NULL));
    if (!g_pw_stream) {
        fprintf(stderr, "audiov: pw_stream_new failed\n");
        return -1;
    }

    memset(&g_stream_hook, 0, sizeof(g_stream_hook));
    pw_stream_add_listener(g_pw_stream, &g_stream_hook, &pw_stream_events, NULL);

    /* Negotiate: mono float32 at CFG_SAMPLE_RATE */
    uint8_t pod_buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));
    const struct spa_pod *params[1];
    struct spa_audio_info_raw audio_info = {
        .format   = SPA_AUDIO_FORMAT_F32,
        .channels = 1,
        .rate     = CFG_SAMPLE_RATE,
    };
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info);

    pw_stream_connect(
        g_pw_stream,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * Xresources — load colour overrides
 * ═══════════════════════════════════════════════════════════════════
 *
 * Supported resources (all optional, compiled defaults used as fallback):
 *
 *   audiov.color          bar colour          #rrggbb
 *   audiov.background     bg colour + alpha   #aarrggbb  (aa=0x00 transparent)
 *   audiov.alpha          bg alpha alone      0-255
 *   *.foreground          fallback bar colour (picks up terminal themes)
 *
 * Example ~/.Xresources:
 *   audiov.color:       #ff4500
 *   audiov.background:  #aa000000
 *   ! or just reuse your terminal foreground:
 *   *.foreground:       #00e5ff
 * ═══════════════════════════════════════════════════════════════════ */

/* Parse #rrggbb or #aarrggbb into component bytes. Returns 1 on success. */
static int xr_parse_color(const char *s, int *r, int *g, int *b, int *a)
{
    if (!s || s[0] != '#') return 0;
    s++;
    unsigned int v[4] = {0, 0, 0, 0};
    int len = (int)strlen(s);
    if (len == 6 && sscanf(s, "%02x%02x%02x", &v[0], &v[1], &v[2]) == 3) {
        *r = (int)v[0]; *g = (int)v[1]; *b = (int)v[2];
        return 1;
    }
    if (len == 8 && sscanf(s, "%02x%02x%02x%02x", &v[0], &v[1], &v[2], &v[3]) == 4) {
        *a = (int)v[0]; *r = (int)v[1]; *g = (int)v[2]; *b = (int)v[3];
        return 1;
    }
    return 0;
}

static void load_xresources(void)
{
    XrmInitialize();
    char *rstr = XResourceManagerString(g_dpy);
    if (!rstr) return;

    XrmDatabase db = XrmGetStringDatabase(rstr);
    if (!db) return;

    char      *type = NULL;
    XrmValue   val;

    /* ── bar colour ───────────────────────────────────────────────── */
    /* Priority: audiov.color > audiov.barColor > *.foreground        */
    static const char * const bar_names[][2] = {
        { "audiov.color",    "Audiov.Color"    },
        { "audiov.barColor", "Audiov.BarColor" },
        { "*.foreground",    "*foreground"     },
        { NULL, NULL }
    };
    for (int i = 0; bar_names[i][0]; i++) {
        if (XrmGetResource(db, bar_names[i][0], bar_names[i][1], &type, &val)
                && val.addr) {
            int dummy_a = g_xr_bar_a;
            if (xr_parse_color(val.addr,
                    &g_xr_bar_r, &g_xr_bar_g, &g_xr_bar_b, &dummy_a)) {
                printf("audiov: bar colour from %%s: %%s\n",
                       bar_names[i][0], val.addr);
                break;
            }
        }
    }

    /* ── background colour / alpha ────────────────────────────────── */
    if (XrmGetResource(db, "audiov.background", "Audiov.Background",
                       &type, &val) && val.addr) {
        xr_parse_color(val.addr,
            &g_xr_bg_r, &g_xr_bg_g, &g_xr_bg_b, &g_xr_bg_a);
        printf("audiov: background from Xresources: %%s\n", val.addr);
    }

    /* ── alpha alone (overrides whatever background set) ─────────── */
    if (XrmGetResource(db, "audiov.alpha", "Audiov.Alpha",
                       &type, &val) && val.addr) {
        int v = atoi(val.addr);
        if (v >= 0 && v <= 255) {
            g_xr_bg_a = v;
            printf("audiov: bg alpha from Xresources: %%d\n", v);
        }
    }

    XrmDestroyDatabase(db);
}

/* ═══════════════════════════════════════════════════════════════════
 * X11 window setup
 * ═══════════════════════════════════════════════════════════════════ */

/* Build a raw 0xAARRGGBB pixel for the 32-bit ARGB visual.
 * No XAllocColor needed - compositor reads the alpha channel directly. */
static unsigned long argb_pixel(int a, int r, int g, int b)
{
    return ((unsigned long)(a & 0xff) << 24)
         | ((unsigned long)(r & 0xff) << 16)
         | ((unsigned long)(g & 0xff) <<  8)
         |  (unsigned long)(b & 0xff);
}

static int setup_x11(void)
{
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "audiov: cannot open display (is DISPLAY set?)\n");
        return -1;
    }

    load_xresources();   /* read colour overrides before anything else */

    g_screen = DefaultScreen(g_dpy);
    g_sw = DisplayWidth(g_dpy,  g_screen);
    g_sh = DisplayHeight(g_dpy, g_screen);

    g_ww = g_sw - 2 * CFG_MARGIN_X;
    g_wh = CFG_WIN_HEIGHT;
    int wx = CFG_MARGIN_X;
    int wy = g_sh - CFG_WIN_HEIGHT - CFG_MARGIN_Y;

    /* Find a 32-bit ARGB visual — required for real transparency.
     * A compositor (picom, compton, kwin, mutter…) must be running. */
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(g_dpy, g_screen, 32, TrueColor, &vinfo)) {
        fprintf(stderr, "audiov: no 32-bit ARGB visual found.\n"
                        "       Make sure a compositor (e.g. picom) is running.\n");
        return -1;
    }
    g_visual = vinfo.visual;
    g_cmap   = XCreateColormap(g_dpy, RootWindow(g_dpy, g_screen),
                               g_visual, AllocNone);

    /* Build colours as raw ARGB pixel values */
    g_col_bg  = argb_pixel(g_xr_bg_a,  g_xr_bg_r,  g_xr_bg_g,  g_xr_bg_b);
    g_col_bar = argb_pixel(g_xr_bar_a, g_xr_bar_r, g_xr_bar_g, g_xr_bar_b);

    /* Create a borderless ARGB window with override-redirect */
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.background_pixel  = 0;          /* start fully transparent */
    attrs.border_pixel      = 0;
    attrs.colormap          = g_cmap;

    g_win = XCreateWindow(
        g_dpy, RootWindow(g_dpy, g_screen),
        wx, wy, (unsigned)g_ww, (unsigned)g_wh, 0,
        32,                                /* 32-bit depth */
        InputOutput,
        g_visual,
        CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWColormap,
        &attrs);

    /* EWMH: declare as a dock so compositors treat it properly */
    {
        Atom net_wm_type  = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE", False);
        Atom type_dock    = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
        XChangeProperty(g_dpy, g_win, net_wm_type, XA_ATOM, 32,
            PropModeReplace, (unsigned char *)&type_dock, 1);
    }

    /* EWMH state: above/below + sticky + skip taskbar/pager */
    {
        Atom net_wm_state  = XInternAtom(g_dpy, "_NET_WM_STATE",              False);
        Atom state_above   = XInternAtom(g_dpy, "_NET_WM_STATE_ABOVE",        False);
        Atom state_below   = XInternAtom(g_dpy, "_NET_WM_STATE_BELOW",        False);
        Atom state_sticky  = XInternAtom(g_dpy, "_NET_WM_STATE_STICKY",       False);
        Atom skip_taskbar  = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
        Atom skip_pager    = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_PAGER",   False);
        Atom layer         = g_on_top ? state_above : state_below;
        Atom states[4]     = { layer, state_sticky, skip_taskbar, skip_pager };
        XChangeProperty(g_dpy, g_win, net_wm_state, XA_ATOM, 32,
            PropModeReplace, (unsigned char *)states, 4);
    }

    /* Class hint so WM can identify us */
    XClassHint *ch = XAllocClassHint();
    if (ch) {
        ch->res_name  = (char *)"audiov";
        ch->res_class = (char *)"Audiov";
        XSetClassHint(g_dpy, g_win, ch);
        XFree(ch);
    }

    g_gc = XCreateGC(g_dpy, g_win, 0, NULL);

    /* Back buffer — we draw here, then blit in one XCopyArea per frame */
    g_pixmap = XCreatePixmap(g_dpy, g_win,
                             (unsigned)g_ww, (unsigned)g_wh, 32);

    XMapWindow(g_dpy, g_win);
    XLowerWindow(g_dpy, g_win);

    /* Make the window completely click-through — an empty input shape means
     * all pointer events fall straight through to whatever is underneath. */
    XShapeCombineRectangles(g_dpy, g_win, ShapeInput, 0, 0,
                            NULL, 0, ShapeSet, 0);

    XFlush(g_dpy);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * Rendering
 * ═══════════════════════════════════════════════════════════════════ */

static void draw_frame(void)
{
    /* All drawing goes to the off-screen pixmap first, then we blit
     * in a single XCopyArea — halves the number of X11 round trips
     * and eliminates per-bar XSetForeground/XFillRectangle flushes. */

    /* Clear pixmap to fully transparent */
    XSetForeground(g_dpy, g_gc, 0x00000000);
    XFillRectangle(g_dpy, g_pixmap, g_gc, 0, 0,
                   (unsigned)g_ww, (unsigned)g_wh);

    int total_slots = CFG_NUM_BARS * 2;
    int bar_slot    = g_ww / total_slots;
    int bar_w       = bar_slot - CFG_BAR_GAP;
    if (bar_w < 1) bar_w = 1;
    int x_offset    = (g_ww - bar_slot * total_slots) / 2;

    XSetForeground(g_dpy, g_gc, g_col_bar);

    for (int i = 0; i < CFG_NUM_BARS; i++) {
        int h = (int)(g_bars[i] * (float)g_wh);
        if (h < 1) continue;

        int y = g_wh - h;

        int xl = x_offset + (CFG_NUM_BARS - 1 - i) * bar_slot;
        XFillRectangle(g_dpy, g_pixmap, g_gc,
            xl, y, (unsigned)bar_w, (unsigned)h);

        int xr = x_offset + (CFG_NUM_BARS + i) * bar_slot;
        XFillRectangle(g_dpy, g_pixmap, g_gc,
            xr, y, (unsigned)bar_w, (unsigned)h);
    }

    /* Single blit to the window */
    XCopyArea(g_dpy, g_pixmap, g_win, g_gc,
              0, 0, (unsigned)g_ww, (unsigned)g_wh, 0, 0);
    /* override_redirect windows are invisible to the WM so EWMH state
     * hints have no effect. For on-top mode we must re-raise every frame
     * ourselves — there is no other mechanism. */
    if (g_on_top)
        XRaiseWindow(g_dpy, g_win);
    XFlush(g_dpy);
}

/* ═══════════════════════════════════════════════════════════════════
 * Signal handling
 * ═══════════════════════════════════════════════════════════════════ */

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_pw_loop) pw_main_loop_quit(g_pw_loop);
}

/* ═══════════════════════════════════════════════════════════════════
 * Entry point
 * ═══════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--on-top") == 0)
            g_on_top = 1;
        else if (strcmp(argv[i], "--no-on-top") == 0)
            g_on_top = 0;
        else {
            fprintf(stderr, "Usage: audiov [-t|--on-top]\n");
            return 1;
        }
    }

    printf("audiov — X11 PipeWire visualizer (Ctrl-C to quit)\n");
    printf("audiov: layer = %s\n", g_on_top ? "above (on top)" : "below (background)");

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    if (setup_x11()      != 0) return 1;
    if (setup_pipewire() != 0) return 1;

    /* Run PipeWire in a background thread */
    pthread_t pw_tid;
    pthread_create(&pw_tid, NULL, pw_thread_fn, NULL);

    /* Work buffers */
    float pcm_copy[CFG_FFT_SIZE];
    float mag[CFG_FFT_SIZE / 2];

    const long    frame_ns       = 1000000000L / CFG_FPS;
    const int     idle_threshold = CFG_IDLE_SECS * CFG_FPS;
    /* XRaiseWindow is expensive — throttle to 10 Hz in on-top mode */
    const int     raise_interval = CFG_FPS / 10;
    int           raise_counter  = 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (g_running) {

        /* ── gamemode check (every CFG_GAMEMODE_CHECK_SECS) ──────── */
        static int gm_check_counter = 0;
        if (++gm_check_counter >= CFG_FPS * CFG_GAMEMODE_CHECK_SECS) {
            gm_check_counter = 0;
            int gm = gamemode_query_status() > 0;
            if (gm && !g_gamemode) {
                g_gamemode = 1;
                if (g_visible) { XUnmapWindow(g_dpy, g_win); XFlush(g_dpy); }
                printf("audiov: gamemode active — suspended\n");
            } else if (!gm && g_gamemode) {
                g_gamemode = 0;
                if (g_visible) {
                    XMapWindow(g_dpy, g_win);
                    if (g_on_top) XRaiseWindow(g_dpy, g_win);
                    else          XLowerWindow(g_dpy, g_win);
                    XFlush(g_dpy);
                }
                printf("audiov: gamemode ended — resuming\n");
            }
        }

        if (g_gamemode) goto next_frame;

        /* ── snapshot ring buffer ──────────────────────────────────── */
        pthread_mutex_lock(&g_mutex);
        int pos = g_pcm_pos;
        memcpy(pcm_copy, g_pcm, sizeof(g_pcm));
        pthread_mutex_unlock(&g_mutex);

        /* ── silence detection (compare squared RMS — avoids sqrtf) ── */
        {
            float sum2 = 0.0f;
            int n = (pos < CFG_FFT_SIZE) ? pos : CFG_FFT_SIZE;
            for (int i = 0; i < n; i++) sum2 += pcm_copy[i] * pcm_copy[i];
            float rms2 = n > 0 ? sum2 / (float)n : 0.0f;
            int audio_present = (rms2 >= CFG_SILENCE_RMS * CFG_SILENCE_RMS);

            if (audio_present) {
                g_silence_frames = 0;
                if (!g_visible) {
                    XMapWindow(g_dpy, g_win);
                    if (g_on_top) XRaiseWindow(g_dpy, g_win);
                    else          XLowerWindow(g_dpy, g_win);
                    XFlush(g_dpy);
                    g_visible = 1;
                    printf("audiov: audio detected, showing\n");
                }
            } else {
                if (g_visible && ++g_silence_frames >= idle_threshold) {
                    XUnmapWindow(g_dpy, g_win);
                    XFlush(g_dpy);
                    g_visible = 0;
                    memset(g_bars,    0, sizeof(g_bars));
                    memset(g_bar_vel, 0, sizeof(g_bar_vel));
                    printf("audiov: silence for %ds, hiding\n", CFG_IDLE_SECS);
                }
            }
        }

        /* ── FFT + render only when visible ────────────────────────── */
        if (g_visible) {
            if (pos >= CFG_FFT_SIZE) {
                int start = pos % CFG_FFT_SIZE;
                float ordered[CFG_FFT_SIZE];
                int tail = CFG_FFT_SIZE - start;
                memcpy(ordered,        pcm_copy + start, (size_t)tail  * sizeof(float));
                memcpy(ordered + tail, pcm_copy,         (size_t)start * sizeof(float));
                fft_forward(ordered, mag, CFG_FFT_SIZE);
            } else {
                float ordered[CFG_FFT_SIZE];
                memset(ordered, 0, sizeof(ordered));
                memcpy(ordered, pcm_copy, (size_t)pos * sizeof(float));
                fft_forward(ordered, mag, CFG_FFT_SIZE);
            }
            update_bars(mag, CFG_FFT_SIZE / 2);
            draw_frame();

            /* Throttled raise — not every frame */
            if (g_on_top && ++raise_counter >= raise_interval) {
                raise_counter = 0;
                XRaiseWindow(g_dpy, g_win);
            }
        }

        /* ── X events ───────────────────────────────────────────────── */
        while (XPending(g_dpy)) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            if (ev.type == Expose || ev.type == MapNotify) {
                if (g_on_top) XRaiseWindow(g_dpy, g_win);
                else          XLowerWindow(g_dpy, g_win);
            }
        }

next_frame:;
        /* ── accurate frame pacing ──────────────────────────────────
         * Advance t0 by exactly one frame period and sleep until then.
         * Processing time is automatically subtracted — no drift. */
        t0.tv_nsec += frame_ns;
        if (t0.tv_nsec >= 1000000000L) {
            t0.tv_nsec -= 1000000000L;
            t0.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t0, NULL);
    }

    printf("\naudiov: shutting down…\n");

    /* Cleanup */
    pw_stream_destroy(g_pw_stream);
    pw_main_loop_quit(g_pw_loop);
    pthread_join(pw_tid, NULL);
    pw_core_disconnect(g_pw_core);
    pw_context_destroy(g_pw_ctx);
    pw_main_loop_destroy(g_pw_loop);
    pw_deinit();

    XFreePixmap(g_dpy, g_pixmap);
    XFreeGC(g_dpy, g_gc);
    XDestroyWindow(g_dpy, g_win);
    XCloseDisplay(g_dpy);

    return 0;
}
