/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// This TU includes LibWebView (via Application.h), so it must stay X11-free: it
// reaches the UltraCanvas application only through the firewall in
// UltraCanvasPlatform.h and the forward-declared base in the bridge header.
#include <LibCore/EventLoop.h>
#include <LibCore/EventLoopImplementation.h>

#include <UI/UltraCanvas/Application.h>
#include <UI/UltraCanvas/EventLoopImplementationUltraCanvas.h>
#include <UI/UltraCanvas/UltraCanvasPlatform.h>

namespace Ladybird {

Application::Application() = default;
Application::~Application() = default;

void Application::create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options)
{
    // This backend presents CPU/shareable bitmaps and has no GPU present path. Force
    // CPU painting so the Compositor uses a software Skia backend: with a GPU backend
    // its flush_async() completes only when the GPU signals, and that completion
    // callback is what actually sends did_present_frame to us — on machines where the
    // GPU/DMABuf path is unavailable it never fires, so no frame is ever presented and
    // the view stays blank. CPU painting makes the flush (and thus presentation)
    // synchronous and reliable.
    web_content_options.force_cpu_painting = WebView::ForceCPUPainting::Yes;

    // Milestone 4 will also wire the settings/config path here (cf. UI/Qt/Application.cpp).
}

Core::EventLoop& Application::create_platform_event_loop()
{
    if (!browser_options().headless_mode.has_value()) {
        // Create and initialize the UltraCanvas application (connects to X11, loads
        // fonts) before installing the event-loop manager, since the manager holds a
        // reference to it and the manager must be installed before the base creates
        // the main Core::EventLoop below. Creation is behind the X11 firewall.
        m_ultracanvas_app = &create_and_initialize_ultracanvas_application("Ladybird");
        Core::EventLoopManager::install(*new EventLoopManagerUltraCanvas(*m_ultracanvas_app));
    }

    auto& event_loop = WebView::Application::create_platform_event_loop();

    if (!browser_options().headless_mode.has_value())
        static_cast<EventLoopImplementationUltraCanvas&>(event_loop.impl()).set_main_loop();

    return event_loop;
}

}
