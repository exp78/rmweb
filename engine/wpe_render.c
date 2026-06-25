/*
 * wpe_render.c -- Render a web page to PNG using WPE WebKit on software GL.
 *
 * Proof that the cross-built WPE WebKit (Skia CPU, software Mesa EGL/GLES,
 * surfaceless, no GPU) actually lays out + paints a page on the reMarkable
 * Paper Pro target (aarch64).
 *
 * Pipeline:
 *   WPEDisplayHeadless  --(EGL_PLATFORM_SURFACELESS_MESA, softpipe)-->
 *   WebKitWebView("display"=headless)  -> load data: URL ->
 *   WPEView::buffer-rendered (WPEBuffer) -> wpe_buffer_import_to_pixels (BGRA)
 *   -> libpng -> /work/build/wpe-render.png
 *
 * Build/run inside rmweb-sdk; see scripts/build-wpe.sh for the exact recipe.
 */
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>
#include <glib.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int   VIEW_W = 800;
static const int   VIEW_H = 600;
static const char *OUT_PNG = "/work/build/wpe-render.png";

static GMainLoop *loop = NULL;
static gboolean   wrote_png = FALSE;
static gboolean   load_done = FALSE;
static int        frames_seen = 0;

/* A simple, deterministic test page: colored boxes + text, no network/fonts beyond system. */
static const char *PAGE =
    "<html><head><meta charset='utf-8'><style>"
    "html,body{margin:0;padding:0}"
    "body{background:#ffffff;font-family:sans-serif;color:#111}"
    ".bar{height:90px;background:#1565c0;color:#fff;display:flex;"
        "align-items:center;padding:0 28px;font-size:34px;font-weight:700}"
    ".box{width:240px;height:160px;margin:30px;border-radius:14px;"
        "display:inline-block;vertical-align:top;color:#fff;font-size:22px;"
        "padding:16px;box-sizing:border-box}"
    ".r{background:#e53935}.g{background:#2e7d32}.y{background:#f9a825;color:#222}"
    "h1{font-size:40px;margin:24px 28px 6px}"
    "p{font-size:20px;margin:6px 28px;line-height:1.4}"
    "</style></head><body>"
    "<div class='bar'>WPE WebKit &mdash; reMarkable Paper Pro</div>"
    "<h1>Software GL render OK</h1>"
    "<p>Skia CPU + Mesa softpipe (surfaceless EGL), no GPU. aarch64.</p>"
    "<div class='box r'>RED<br>box</div>"
    "<div class='box g'>GREEN<br>box</div>"
    "<div class='box y'>YELLOW<br>box</div>"
    "<p>The quick brown fox jumps over the lazy dog. 0123456789.</p>"
    "</body></html>";

/* Write a BGRA/BGRX buffer (little-endian ARGB8888 DRM => B,G,R,A in memory) to RGBA PNG. */
static gboolean write_png_bgra(const char *path, const guint8 *data,
                               int width, int height, int stride)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "fopen %s failed\n", path); return FALSE; }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) { fprintf(stderr, "libpng init failed\n"); if (fp) fclose(fp); return FALSE; }
    if (setjmp(png_jmpbuf(png))) {
        fprintf(stderr, "libpng longjmp error\n");
        png_destroy_write_struct(&png, &info); fclose(fp); return FALSE;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    png_bytep row = (png_bytep)malloc((size_t)width * 4);
    if (!row) {
        fprintf(stderr, "write_png_bgra: OOM (%d px row)\n", width);
        png_destroy_write_struct(&png, &info); fclose(fp); return FALSE;
    }
    for (int y = 0; y < height; ++y) {
        const guint8 *src = data + (size_t)y * stride;
        for (int x = 0; x < width; ++x) {
            /* memory order from GBM map of ARGB8888 little-endian == B,G,R,A */
            guint8 b = src[x*4 + 0];
            guint8 g = src[x*4 + 1];
            guint8 r = src[x*4 + 2];
            guint8 a = src[x*4 + 3];
            row[x*4 + 0] = r;
            row[x*4 + 1] = g;
            row[x*4 + 2] = b;
            row[x*4 + 3] = a ? a : 0xff; /* XRGB has no alpha; force opaque */
        }
        png_write_row(png, row);
    }
    free(row);
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return TRUE;
}

static void on_buffer_rendered(WPEView *view, WPEBuffer *buffer, gpointer user_data)
{
    (void)view; (void)user_data;
    if (wrote_png)
        return;                  /* already captured; ignore frames drained after quit */
    frames_seen++;
    int w = wpe_buffer_get_width(buffer);
    int h = wpe_buffer_get_height(buffer);
    g_printerr("[render] buffer-rendered #%d  %dx%d  (%s)\n", frames_seen, w, h,
               G_OBJECT_TYPE_NAME(buffer));

    /* Capture only after the load FINISHED and at least one further frame, so the buffer
       reflects the painted page rather than the initial blank surface. */
    if (!load_done || frames_seen < 2)
        return;
    if (w <= 0 || h <= 0) {      /* dimensions not propagated yet -> avoid /0 and a NULL row to libpng */
        g_printerr("[render] skip: buffer not sized yet (%dx%d)\n", w, h);
        return;
    }

    GError *err = NULL;
    GBytes *bytes = wpe_buffer_import_to_pixels(buffer, &err);
    if (!bytes) {
        g_printerr("[render] import_to_pixels failed: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    gsize size = 0;
    const guint8 *pix = g_bytes_get_data(bytes, &size);
    int stride = (int)(size / (gsize)h);
    g_printerr("[render] pixels: %zu bytes, stride=%d (=%d bpp)\n", size, stride, stride / w);

    if (write_png_bgra(OUT_PNG, pix, w, h, stride)) {
        g_printerr("[render] wrote %s (%dx%d)\n", OUT_PNG, w, h);
        wrote_png = TRUE;
    }
    g_bytes_unref(bytes);
    if (loop) g_main_loop_quit(loop);
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent ev, gpointer user_data)
{
    (void)view; (void)user_data;
    if (ev == WEBKIT_LOAD_FINISHED) {
        load_done = TRUE;
        g_printerr("[render] load finished\n");
    }
}

static gboolean on_timeout(gpointer user_data)
{
    (void)user_data;
    g_printerr("[render] TIMEOUT after wait; frames_seen=%d wrote=%d\n", frames_seen, wrote_png);
    if (loop) g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;  /* output path is fixed to OUT_PNG */

    /* 1. Headless WPE display (surfaceless software EGL under the hood). */
    WPEDisplay *display = wpe_display_headless_new();
    if (!display) { g_printerr("FAIL: wpe_display_headless_new returned NULL\n"); return 2; }

    GError *err = NULL;
    if (!wpe_display_connect(display, &err)) {
        g_printerr("FAIL: wpe_display_connect: %s\n", err ? err->message : "?");
        return 2;
    }
    g_printerr("[render] headless display connected: %s\n", G_OBJECT_TYPE_NAME(display));

    /* 2. WebKitWebView bound to that display (WPEPlatform path). */
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW,
        "display", display,
        NULL));
    if (!web_view) { g_printerr("FAIL: web view creation\n"); return 2; }

    /* white default so transparent areas are visible/opaque */
    WebKitColor bg;
    if (webkit_color_parse(&bg, "#ffffff"))
        webkit_web_view_set_background_color(web_view, &bg);

    /* 3. Get the internal WPEView and sit on its buffer-rendered signal. */
    WPEView *wpe_view = webkit_web_view_get_wpe_view(web_view);
    if (!wpe_view) { g_printerr("FAIL: get_wpe_view NULL\n"); return 2; }
    g_printerr("[render] wpe_view: %s\n", G_OBJECT_TYPE_NAME(wpe_view));

    /* Force a concrete size via the toplevel + view. */
    WPEToplevel *top = wpe_view_get_toplevel(wpe_view);
    if (top) wpe_toplevel_resize(top, VIEW_W, VIEW_H);
    wpe_view_resized(wpe_view, VIEW_W, VIEW_H);

    g_signal_connect(wpe_view, "buffer-rendered", G_CALLBACK(on_buffer_rendered), NULL);
    g_signal_connect(web_view, "load-changed", G_CALLBACK(on_load_changed), NULL);

    /* 4. Load the test page and pump the loop. */
    webkit_web_view_load_html(web_view, PAGE, NULL);

    loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(45, on_timeout, NULL);
    g_main_loop_run(loop);

    g_clear_object(&web_view);
    g_clear_object(&display);

    if (!wrote_png) { g_printerr("FAIL: no PNG written\n"); return 1; }
    g_printerr("[render] SUCCESS\n");
    return 0;
}
