/*
 * wpe_cadence.c -- Measure the WPE "buffer-rendered" cadence on the MAIN thread, no Qt.
 *
 * Why: the Qt app (engine/wpeqt/main.cpp) sees buffer-rendered fire on a ~6 s grid even though the
 * WPE source says the headless render clock is a 60 fps software timer (displayID 0). This probe
 * removes Qt and the worker-thread/GMainContext split entirely: it creates the headless WebKitWebView
 * on the process main thread, runs ONE GMainLoop on the default context, maps the view exactly like the
 * app, then every 2 s does scrollBy + a 1-char DOM mutation (our repaint trigger) and logs the interval
 * between buffer-rendered signals. If the cadence here is fast (tens of ms) -> the app's threading is the
 * gate. If it is still ~6 s -> the gate is intrinsic to WPE/WebKit on this device.
 *
 * Build/run via scripts/build-wpe.sh cadence (see engine/render-wpe.incontainer.sh).
 */
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>
#include <glib.h>
#include <stdio.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

static void crash_handler(int sig)
{
    void *bt[64];
    int n = backtrace(bt, 64);
    fprintf(stderr, "\n[cad][CRASH] signal %d (%d frames):\n", sig, n);
    backtrace_symbols_fd(bt, n, fileno(stderr));
    fflush(stderr);
    signal(sig, SIG_DFL);
    raise(sig);
}

static const int VIEW_W = 1620, VIEW_H = 2160;

static GMainLoop *loop = NULL;
static WebKitWebView *web_view = NULL;
static gint64 start_us = 0, last_buf_us = 0;
static int frames = 0, dir = 1;

static double ms_since(gint64 us) { return (g_get_monotonic_time() - us) / 1000.0; }

static void on_buffer_rendered(WPEView *view, WPEBuffer *buffer, gpointer u)
{
    (void)view; (void)u;
    gint64 now = g_get_monotonic_time();
    double dt = last_buf_us ? (now - last_buf_us) / 1000.0 : 0.0;
    last_buf_us = now;
    g_printerr("[cad] frame %d @%.0fms  dt=%.1fms  %dx%d\n", ++frames, ms_since(start_us), dt,
               wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer));
}

/* Every 2 s: scroll one screen and force a repaint (same trick as the app). */
static gboolean on_scroll_tick(gpointer u)
{
    (void)u;
    int dy = dir * 1500; dir = -dir;
    char *js = g_strdup_printf("window.scrollBy(0,%d);", dy);
    webkit_web_view_evaluate_javascript(web_view, js, -1, NULL, NULL, NULL, NULL, NULL);
    g_free(js);
    /* FORCE a fresh commit the same way the (fast) initial paint happens: re-map the view
     * (set_visible FALSE->TRUE). This is the only thing seen to bypass the ~6 s buffer clock. No reflow. */
    WPEView *v = webkit_web_view_get_wpe_view(web_view);
    wpe_view_set_visible(v, FALSE);
    wpe_view_set_visible(v, TRUE);
    g_printerr("[cad] scrollBy(%d)+remap @%.0fms\n", dy, ms_since(start_us));
    return G_SOURCE_CONTINUE;
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent ev, gpointer u)
{
    (void)view; (void)u;
    if (ev == WEBKIT_LOAD_FINISHED) {
        g_printerr("[cad] load finished @%.0fms\n", ms_since(start_us));
        g_timeout_add(3000, on_scroll_tick, NULL);   /* start the scroll cadence */
    }
}

static gboolean on_timeout(gpointer u) { (void)u; if (loop) g_main_loop_quit(loop); return G_SOURCE_REMOVE; }

/* Page carries a continuous CSS transform animation (RMWEB_ANIM=1) to test whether the compositor can
 * produce frames faster than ~6 s when something animates continuously. */
static const char *PAGE =
    "<html><head><meta charset='utf-8'><style>"
    "html,body{margin:0;padding:0;font-family:sans-serif}"
    "p{font-size:32px;margin:18px}</style></head><body><div id='c'></div>"
    "<script>var h='';for(var i=1;i<=80;i++)h+='<p>Line '+i+' the quick brown fox 0123456789.</p>';"
    "document.getElementById('c').innerHTML=h;</script></body></html>";

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stderr, NULL, _IOLBF, 0);   /* line-buffer so the log file survives a kill/crash */
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    start_us = g_get_monotonic_time();

    WPEDisplay *display = wpe_display_headless_new();
    GError *err = NULL;
    if (!display || !wpe_display_connect(display, &err)) {
        g_printerr("FAIL: display connect: %s\n", err ? err->message : "?"); return 2;
    }
    g_printerr("[cad] display connected, n_screens=%u @%.0fms\n",
               wpe_display_get_n_screens(display), ms_since(start_us));

    web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display, NULL));
    WPEView *wpe_view = webkit_web_view_get_wpe_view(web_view);

    /* Map the view exactly like the app: size the toplevel, then force visible FALSE->TRUE. */
    WPEToplevel *top = wpe_view_get_toplevel(wpe_view);
    if (top) wpe_toplevel_resize(top, VIEW_W, VIEW_H);
    wpe_view_resized(wpe_view, VIEW_W, VIEW_H);
    g_signal_connect(wpe_view, "buffer-rendered", G_CALLBACK(on_buffer_rendered), NULL);
    wpe_view_set_visible(wpe_view, FALSE);
    wpe_view_set_visible(wpe_view, TRUE);
    g_printerr("[cad] view mapped=%d @%.0fms\n", wpe_view_get_mapped(wpe_view), ms_since(start_us));

    g_signal_connect(web_view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    webkit_web_view_load_html(web_view, PAGE, NULL);

    loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(24, on_timeout, NULL);
    g_main_loop_run(loop);
    g_printerr("[cad] done, frames=%d\n", frames);
    return 0;
}
