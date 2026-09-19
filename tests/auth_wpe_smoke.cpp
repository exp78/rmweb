// Offline, synthetic form only. This test never loads an authorization page.
#include "../engine/wpeqt/auth-policy.h"
#include <QCoreApplication>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
GMainLoop *loop;
WebKitWebView *page;
rmweb::AuthKeyboard keyboard;
int result = 1;
int phase = 0;

void finish(bool okay, const char *message) {
    result = okay ? 0 : 1;
    std::fprintf(okay ? stdout : stderr, "%s\n", message);
    g_main_loop_quit(loop);
}

void key(int raw) {
    WPEView *view = webkit_web_view_get_wpe_view(page);
    for (bool pressed : {true, false}) {
        for (const auto &event : keyboard.event(raw, pressed)) {
            WPEEvent *packet = wpe_event_keyboard_new(
                event.pressed ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP,
                view, WPE_INPUT_SOURCE_KEYBOARD, guint32(g_get_monotonic_time() / 1000),
                WPEModifiers(event.key.modifiers), event.key.code, event.key.value);
            wpe_view_event(view, packet);
            wpe_event_unref(packet);
        }
    }
}

void evaluated(GObject *source, GAsyncResult *async, gpointer) {
    GError *error = nullptr;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), async, &error);
    if (!value || error) {
        g_clear_error(&error);
        if (value) g_object_unref(value);
        finish(false, "FAIL synthetic page evaluation");
        return;
    }
    gchar *text = jsc_value_to_string(value);
    const char *expected = phase == 0 ? "focused" : phase == 1 ? "[\"aB@\",0,\"Digit2\"]"
        : phase == 2 ? "[\"aBc\",1,\"Enter\"]" : "[\"d\",1,\"KeyD\"]";
    const bool matches = text && std::strcmp(text, expected) == 0;
    g_free(text);
    g_object_unref(value);
    if (!matches) { finish(false, "FAIL synthetic input value or submit count"); return; }
    if (phase == 3) {
        finish(true, "PASS actual WPE synthetic form: letters, Shift, symbol, Backspace, Enter, Ctrl+A");
        return;
    }
    if (phase == 0) {
        key('A'); key(0x100000 | 'B'); key('@');
    } else if (phase == 1) {
        key(128); key('C'); key(13);
    } else {
        key(0x200000 | 'A'); key('D');
    }
    ++phase;
    g_timeout_add(250, [](gpointer) -> gboolean {
        constexpr auto script = "JSON.stringify([document.getElementById('field').value, window.submits, window.lastCode])";
        webkit_web_view_evaluate_javascript(page, script, -1, nullptr, nullptr, nullptr, evaluated, nullptr);
        return G_SOURCE_REMOVE;
    }, nullptr);
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    loop = g_main_loop_new(nullptr, FALSE);
    GError *error = nullptr;
    WPEDisplay *display = wpe_display_headless_new();
    if (!display || !wpe_display_connect(display, &error)) {
        g_clear_error(&error);
        std::fputs("FAIL headless display setup\n", stderr);
        return 1;
    }
    WebKitNetworkSession *session = webkit_network_session_new_ephemeral();
    page = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
        "network-session", session, nullptr));
    g_object_unref(display);
    g_object_unref(session);
    WPEView *view = webkit_web_view_get_wpe_view(page);
    if (WPEToplevel *top = wpe_view_get_toplevel(view)) wpe_toplevel_resize(top, 810, 1000);
    wpe_view_resized(view, 810, 1000);
    wpe_view_set_visible(view, TRUE);
    wpe_view_focus_in(view);
    g_signal_connect(page, "load-changed", G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent event, gpointer) {
        if (event != WEBKIT_LOAD_FINISHED) return;
        constexpr auto focus = "document.getElementById('field').focus(); 'focused'";
        webkit_web_view_evaluate_javascript(page, focus, -1, nullptr, nullptr, nullptr, evaluated, nullptr);
    }), nullptr);
    g_signal_connect(page, "web-process-terminated", G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason, gpointer) {
        finish(false, "FAIL synthetic web process terminated");
    }), nullptr);
    constexpr auto html = "<!doctype html><meta charset=utf-8>"
        "<form onsubmit='window.submits++;return false'><input id=field><button>Go</button></form>"
        "<script>window.submits=0;addEventListener('keydown', e => {window.lastCode=e.code})</script>";
    webkit_web_view_load_html(page, html, "about:blank");
    g_timeout_add_seconds(15, [](gpointer) -> gboolean {
        finish(false, "FAIL bounded synthetic form timeout");
        return G_SOURCE_REMOVE;
    }, nullptr);
    g_main_loop_run(loop);
    // Match the disposable browser's process-lifetime teardown. No persistent state.
    std::fflush(nullptr);
    std::_Exit(result);
}
