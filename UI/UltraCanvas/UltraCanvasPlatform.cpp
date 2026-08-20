/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// This TU is the X11 side of the firewall (see UltraCanvasPlatform.h): it includes
// the X11-pulling UltraCanvas headers but MUST NOT include any LibWebView header.
#include <AK/OwnPtr.h>
#include <cstdio>

#include <UI/UltraCanvas/UltraCanvasPlatform.h>

#include <memory>

#include <UltraCanvasApplication.h>
#include <UltraCanvasMenu.h>
#include <UltraCanvasModalDialog.h>
#include <UltraCanvasNativeDialogs.h>
#include <UltraCanvasWindow.h>

namespace Ladybird {

// Owns the single UltraCanvas application for the process lifetime. Held here (where
// the type is complete) rather than in Ladybird::Application, whose TU is X11-free.
static OwnPtr<UltraCanvas::UltraCanvasApplicationBase> s_ultracanvas_app;

UltraCanvas::UltraCanvasApplicationBase& create_and_initialize_ultracanvas_application(char const* app_name)
{
    s_ultracanvas_app = make<UltraCanvas::UltraCanvasApplication>();
    s_ultracanvas_app->Initialize(app_name);
    return *s_ultracanvas_app;
}

void open_placeholder_window(char const* title, int width, int height)
{
    UltraCanvas::WindowConfig config;
    config.title = title;
    config.width = width;
    config.height = height;

    // CreateWindow() registers the window with the current UltraCanvas application,
    // so it stays alive (and keeps Run() iterating) until closed.
    if (auto window = UltraCanvas::CreateWindow(config))
        window->Show();
}

// The dialogs parent themselves to the focused browser window so they are modal to it.
static UltraCanvas::UltraCanvasWindowBase* focused_window()
{
    auto* app = UltraCanvas::UltraCanvasApplication::GetInstance();
    return app ? app->GetFocusedWindow() : nullptr;
}

// If a dialog ever fails to show (exception from the toolkit), we must still invoke
// the callback — otherwise WebContent's blocked script (alert/confirm/prompt) never
// resumes and the page hangs. on_close is captured by copy so the fallback can run it.
void show_alert_dialog(std::string const& message, std::function<void()> on_close)
{
    try {
        UltraCanvas::UltraCanvasDialogManager::ShowInformation(
            message, "Ladybird",
            [on_close](UltraCanvas::DialogResult) {
                if (on_close)
                    on_close();
            },
            focused_window());
    } catch (...) {
        if (on_close)
            on_close();
    }
}

void show_confirm_dialog(std::string const& message, std::function<void(bool)> on_close)
{
    try {
        UltraCanvas::UltraCanvasDialogManager::ShowConfirmation(
            message, "Ladybird",
            [on_close](bool confirmed) {
                if (on_close)
                    on_close(confirmed);
            },
            focused_window());
    } catch (...) {
        if (on_close)
            on_close(false);
    }
}

void show_prompt_dialog(std::string const& message, std::string const& default_value,
    std::function<void(bool, std::string const&)> on_close)
{
    try {
        UltraCanvas::UltraCanvasDialogManager::ShowInputDialog(
            message, "Ladybird", default_value, UltraCanvas::InputType::Text,
            [on_close](UltraCanvas::DialogResult result, std::string const& text) {
                if (on_close)
                    on_close(result == UltraCanvas::DialogResult::OK, text);
            },
            focused_window());
    } catch (...) {
        if (on_close)
            on_close(false, std::string {});
    }
}

// ===== CONTEXT MENUS =====
// OpenPopup() stores the menu by raw pointer, so it must stay alive while shown. One
// context menu is up at a time; hold it here and release on close (deferred, since we
// clear from within the menu's own close callback).
static std::shared_ptr<UltraCanvas::UltraCanvasMenu> s_context_menu;
// Same lifetime rule for the HTML <select> dropdown popup.
static std::shared_ptr<UltraCanvas::UltraCanvasMenu> s_select_dropdown;

static UltraCanvas::MenuItemData to_menu_item(ContextMenuItem const& item)
{
    using UltraCanvas::MenuItemData;

    if (item.is_separator)
        return MenuItemData::Separator();

    if (!item.submenu.empty()) {
        std::vector<MenuItemData> children;
        children.reserve(item.submenu.size());
        for (auto const& child : item.submenu)
            children.push_back(to_menu_item(child));
        auto data = MenuItemData::Submenu(item.label, children);
        data.enabled = item.enabled;
        return data;
    }

    MenuItemData data = item.checkable
        ? MenuItemData::Checkbox(item.label, item.checked, [callback = item.on_activate](bool) { if (callback) callback(); })
        : MenuItemData::Action(item.label, item.on_activate ? item.on_activate : [] { });
    data.enabled = item.enabled;
    return data;
}

void show_context_menu(std::vector<ContextMenuItem> items, int window_x, int window_y)
{
    auto* window = focused_window();
    if (!window)
        return;

    auto menu = std::make_shared<UltraCanvas::UltraCanvasMenu>("web-context-menu");
    menu->SetMenuType(UltraCanvas::MenuType::PopupMenu);
    menu->onMenuClosed = [] {
        // Release the held menu after this close callback returns.
        if (auto* app = UltraCanvas::UltraCanvasApplication::GetInstance())
            app->PostToUIThread([] { s_context_menu.reset(); });
    };

    for (auto const& item : items)
        menu->AddItem(to_menu_item(item));

    UltraCanvas::PopupElementSettings settings;
    settings.closeByEscapeKey = true;
    settings.closeByClickOutside = true;

    s_context_menu = menu;
    menu->OpenMenu(UltraCanvas::Point2Di(window_x, window_y), *window, settings);
}

void show_select_dropdown(std::vector<ContextMenuItem> items, int window_x, int window_y, int minimum_width, std::function<void()> on_dismiss)
{
    auto* window = focused_window();
    if (!window) {
        // Can't show the popup — unblock the engine so the <select> isn't left waiting.
        if (on_dismiss)
            on_dismiss();
        return;
    }

    // Report either the chosen option OR a dismissal, never both: each item's activation sets
    // this flag, and the close handler only reports a dismissal if nothing was picked.
    auto picked = std::make_shared<bool>(false);
    for (auto& item : items) {
        if (item.on_activate) {
            auto activate = item.on_activate;
            item.on_activate = [picked, activate] {
                *picked = true;
                activate();
            };
        }
    }

    auto menu = std::make_shared<UltraCanvas::UltraCanvasMenu>("web-select-dropdown");
    menu->SetMenuType(UltraCanvas::MenuType::PopupMenu);
    // Make the dropdown at least as wide as the <select> control it drops from.
    if (minimum_width > 0) {
        auto style = menu->GetStyle();
        style.minWidth = minimum_width;
        menu->SetStyle(style);
    }
    menu->onMenuClosed = [picked, on_dismiss] {
        // Defer to the next UI turn: an item's on_activate runs synchronously before the menu
        // closes, so by the time this fires `picked` is settled. Also releases the held popup.
        if (auto* app = UltraCanvas::UltraCanvasApplication::GetInstance())
            app->PostToUIThread([picked, on_dismiss] {
                if (!*picked && on_dismiss)
                    on_dismiss();
                s_select_dropdown.reset();
            });
    };

    for (auto const& item : items)
        menu->AddItem(to_menu_item(item));

    UltraCanvas::PopupElementSettings settings;
    settings.closeByEscapeKey = true;
    settings.closeByClickOutside = true;

    s_select_dropdown = menu;
    menu->OpenMenu(UltraCanvas::Point2Di(window_x, window_y), *window, settings);
}

std::string show_save_file_dialog(std::string const& title, std::string const& initial_directory, std::string const& suggested_name)
{
    // Blocking GTK save dialog (returns "" on cancel or if GTK can't initialize). No filters —
    // the caller downloads whatever the link points at.
    return UltraCanvas::UltraCanvasNativeDialogs::SaveFile(title, {}, initial_directory, suggested_name, nullptr);
}

std::string show_open_file_dialog(std::string const& title, std::string const& initial_directory)
{
    // Blocking GTK open dialog (returns "" on cancel or if GTK can't initialize).
    return UltraCanvas::UltraCanvasNativeDialogs::OpenFile(title, {}, initial_directory, nullptr);
}

}
