// Lightweight X11/GTK overlay for Linux Gaming Mode. It appears only while a
// Gaming app is in the G+ state, giving players a clear "Vietnamese is active"
// signal without stealing focus.
#include <gtk/gtk.h>

#include "app.h"

namespace vietki::lin {

namespace {

GtkWidget* g_window = nullptr;
GtkWidget* g_label = nullptr;

void onRealize(GtkWidget* widget, gpointer) {
    GdkWindow* window = gtk_widget_get_window(widget);
    if (window) gdk_window_set_pass_through(window, TRUE);
}

void positionOverlay() {
    if (!g_window) return;

    GdkDisplay* display = gdk_display_get_default();
    GdkMonitor* monitor = display ? gdk_display_get_primary_monitor(display) : nullptr;
    if (!monitor && display && gdk_display_get_n_monitors(display) > 0)
        monitor = gdk_display_get_monitor(display, 0);

    GdkRectangle area{0, 0, 1280, 720};
    if (monitor) gdk_monitor_get_workarea(monitor, &area);

    int width = 150;
    int height = 40;
    gtk_window_resize(GTK_WINDOW(g_window), width, height);

    const int margin = 28;
    int corner = state().config.gamingOverlayCorner;
    int x = (corner == 1 || corner == 3) ? area.x + area.width - width - margin
                                         : area.x + margin;
    int y = (corner == 2 || corner == 3) ? area.y + area.height - height - margin
                                         : area.y + margin;
    gtk_window_move(GTK_WINDOW(g_window), x, y);
}

} // namespace

void initGamingOverlay() {
    if (g_window) return;

    g_window = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_decorated(GTK_WINDOW(g_window), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(g_window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(g_window), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    gtk_widget_set_opacity(g_window, 0.86);
    g_signal_connect(g_window, "realize", G_CALLBACK(onRealize), nullptr);

    GtkWidget* frame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
    gtk_container_add(GTK_CONTAINER(g_window), frame);

    g_label = gtk_label_new("Tiếng Việt");
    gtk_widget_set_size_request(g_label, 150, 40);
    gtk_container_add(GTK_CONTAINER(frame), g_label);

    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(
        css,
        "window, frame { background: rgba(24, 28, 36, 0.86); border-radius: 8px; }"
        "label { color: white; font: 700 14px 'Segoe UI', 'Inter', sans-serif; }",
        -1,
        nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    gtk_widget_show_all(g_window);
    gtk_widget_hide(g_window);
}

void destroyGamingOverlay() {
    if (!g_window) return;
    gtk_widget_destroy(g_window);
    g_window = nullptr;
    g_label = nullptr;
}

void updateGamingOverlay() {
    if (!g_window) return;
    bool show = state().config.gamingOverlayEnabled &&
                state().currentIcon == IconState::GamingVN;
    if (show) {
        positionOverlay();
        gtk_widget_show_all(g_window);
    } else {
        gtk_widget_hide(g_window);
    }
}

} // namespace vietki::lin
