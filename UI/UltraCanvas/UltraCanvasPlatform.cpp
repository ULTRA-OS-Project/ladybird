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

#include <UltraCanvasApplication.h>
#include <UltraCanvasModalDialog.h>
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

}
