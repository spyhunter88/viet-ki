// AppIndicator status menu for the VietKi Linux shell (guide 6, Phase 2 E).
// Analogue of the Windows tray and the macOS status-bar menu. The panel label
// shows the resolved state (V / E / V+ / V-) since custom colour icons are not
// portable across desktop panels; a themed keyboard icon backs it up.
//
// NOTE: mirrors the verified Windows build; not compiled on Linux here.
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>

#include "app.h"

namespace vietki::lin {

namespace {

AppIndicator* g_indicator = nullptr;
bool g_building = false;   // suppress signal handlers while rebuilding the menu

// --- menu callbacks -------------------------------------------------------
void onToggle(GtkMenuItem*, gpointer)    { if (!g_building) toggleEnabled(); }
void onOverride(GtkMenuItem*, gpointer)  { if (!g_building) toggleOverrideForCurrentApp(); }
void onExclusion(GtkMenuItem*, gpointer) { if (!g_building) toggleExclusionFeature(); }
void onTelex(GtkMenuItem*, gpointer)     { if (!g_building) setMethod(Method::Telex); }
void onVni(GtkMenuItem*, gpointer)       { if (!g_building) setMethod(Method::VNI); }
void onModern(GtkMenuItem*, gpointer)    { if (!g_building) setTonePlacement(TonePlacement::Modern); }
void onOld(GtkMenuItem*, gpointer)       { if (!g_building) setTonePlacement(TonePlacement::Old); }

void onAutostart(GtkMenuItem* it, gpointer) {
    if (g_building) return;
    state().config.autostart =
        gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(it));
    saveConfig(state().config);
    applyAutostart(state().config.autostart);
}

void onGamingDisabled(GtkMenuItem*, gpointer) {
    if (!g_building) applyGamingPolicy(GamingPolicy::Disabled);
}
void onGamingToggle(GtkMenuItem*, gpointer) {
    if (!g_building) applyGamingPolicy(GamingPolicy::ToggleForCurrentApp);
}
void onGamingTrigger(GtkMenuItem*, gpointer) {
    if (!g_building) applyGamingPolicy(GamingPolicy::TemporaryTrigger);
}
void onGamingSound(GtkMenuItem* it, gpointer) {
    if (g_building) return;
    state().config.soundOnGamingModeSwitch =
        gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(it));
    saveConfig(state().config);
}
void onGamingOverlay(GtkMenuItem* it, gpointer) {
    if (g_building) return;
    state().config.gamingOverlayEnabled =
        gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(it));
    saveConfig(state().config);
    updateGamingOverlay();
}

// Phase 4 H.5: the two word-handling toggles. Linux has no settings window, so
// they live in the tray menu with their full descriptive names.
void onSpell(GtkMenuItem* it, gpointer) {
    if (g_building) return;
    state().config.spellCheck =
        gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(it));
    saveConfig(state().config);
    applyResolvedState();
}
void onLockCancel(GtkMenuItem* it, gpointer) {
    if (g_building) return;
    state().config.lockWordAfterCancel =
        gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(it));
    saveConfig(state().config);
    applyResolvedState();
}
void onQuit(GtkMenuItem*, gpointer)      { gtk_main_quit(); }

GtkWidget* addItem(GtkWidget* menu, const char* label, GCallback cb) {
    GtkWidget* item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", cb, nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

GtkWidget* addCheck(GtkWidget* menu, const char* label, bool checked, GCallback cb) {
    GtkWidget* item = gtk_check_menu_item_new_with_label(label);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), checked);
    g_signal_connect(item, "activate", cb, nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

void addSeparator(GtkWidget* menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

GtkWidget* buildMenu() {
    AppConfig& c = state().config;
    GtkWidget* menu = gtk_menu_new();

    addItem(menu, c.enabled ? "Tắt tiếng Việt" : "Bật tiếng Việt", G_CALLBACK(onToggle));
    addItem(menu, "Đảo chế độ cho app hiện tại", G_CALLBACK(onOverride));
    addCheck(menu, "Bật tính năng loại trừ", c.exclusionFeatureOn, G_CALLBACK(onExclusion));
    addCheck(menu, "Khởi động cùng hệ thống", c.autostart, G_CALLBACK(onAutostart));
    addSeparator(menu);

    GtkWidget* subItem = gtk_menu_item_new_with_label("Chế độ chơi game");
    GtkWidget* sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(subItem), sub);

    addCheck(sub, "Không dùng", c.gamingPolicy == GamingPolicy::Disabled, G_CALLBACK(onGamingDisabled));
    addCheck(sub, "Bật đổi V-/V+ cho app game", c.gamingPolicy == GamingPolicy::ToggleForCurrentApp, G_CALLBACK(onGamingToggle));
    addCheck(sub, "Bật gõ tiếng Việt tạm thời", c.gamingPolicy == GamingPolicy::TemporaryTrigger, G_CALLBACK(onGamingTrigger));
    addSeparator(sub);
    addCheck(sub, "Bật âm thanh khi chuyển chế độ", c.soundOnGamingModeSwitch, G_CALLBACK(onGamingSound));
    addCheck(sub, "Hiện overlay khi gõ tiếng Việt", c.gamingOverlayEnabled, G_CALLBACK(onGamingOverlay));

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), subItem);
    addSeparator(menu);

    addCheck(menu, "Kiểu gõ: Telex", c.method == Method::Telex, G_CALLBACK(onTelex));
    addCheck(menu, "Kiểu gõ: VNI", c.method == Method::VNI, G_CALLBACK(onVni));
    addSeparator(menu);

    addCheck(menu, "Đặt dấu: Kiểu mới", c.tone == TonePlacement::Modern, G_CALLBACK(onModern));
    addCheck(menu, "Đặt dấu: Kiểu cũ", c.tone == TonePlacement::Old, G_CALLBACK(onOld));
    addSeparator(menu);

    addCheck(menu, "Kiểm tra và khôi phục từ không phải tiếng Việt", c.spellCheck,
             G_CALLBACK(onSpell));
    addCheck(menu, "Giữ phần còn lại của từ sau khi hủy dấu", c.lockWordAfterCancel,
             G_CALLBACK(onLockCancel));
    addSeparator(menu);

    addItem(menu, "Thoát", G_CALLBACK(onQuit));

    gtk_widget_show_all(menu);
    return menu;
}

} // namespace

bool createTray() {
    g_indicator = app_indicator_new("vietki", "input-keyboard",
                                    APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    if (!g_indicator) return false;
    app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_ACTIVE);
    updateTrayIcon();
    return true;
}

void updateTrayIcon() {
    if (!g_indicator) return;
    const char* label = "V";
    switch (state().currentIcon) {
        case IconState::E:        label = "E";  break;
        case IconState::VPlus:    label = "V+"; break;
        case IconState::VMinus:   label = "V-"; break;
        case IconState::V:        label = "V";  break;
        case IconState::Gaming:   label = "G";  break;
        case IconState::GamingVN: label = "G+"; break;
    }
    app_indicator_set_label(g_indicator, label, "V+");

    g_building = true;             // set_active() below would otherwise re-fire
    GtkWidget* menu = buildMenu();
    app_indicator_set_menu(g_indicator, GTK_MENU(menu));
    g_building = false;
}

} // namespace vietki::lin

