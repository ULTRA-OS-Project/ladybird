/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// NOTE (Milestone 3 — MVP render + input):
// Bootstraps the Ladybird::Application (which installs the UltraCanvas ↔
// Core::EventLoop bridge), creates a WebContentView, opens a BrowserWindow hosting
// it, and navigates to the first command-line URL (or about:blank).
//
// This TU is on the LibWebView side of the X11 firewall (see UltraCanvasPlatform.h):
// it creates the view via the X11-free WebViewController factory and opens the
// window via the X11-free open_browser_window() declaration.
#include <AK/String.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>

#include <UI/UltraCanvas/Application.h>
#include <UI/UltraCanvas/BrowserWindow.h>
#include <UI/UltraCanvas/WebViewController.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto app = TRY(Ladybird::Application::create(arguments));

    if (!WebView::Application::browser_options().headless_mode.has_value()) {
        auto const& urls = WebView::Application::browser_options().urls;
        String initial_url = urls.is_empty() ? "about:blank"_string : urls.first().serialize();

        auto view = Ladybird::create_web_content_view();
        Ladybird::open_browser_window(view, initial_url.bytes_as_string_view());
    }

    return app->execute();
}
