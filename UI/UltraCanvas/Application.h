/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Application.h>

// Forward-declared to keep this header free of the UltraCanvas (and X11) headers.
namespace UltraCanvas {
class UltraCanvasApplicationBase;
}

namespace Ladybird {

class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    virtual ~Application() override;

    // The UltraCanvas application instance that owns the toolkit event loop. Created
    // in create_platform_event_loop(); valid for the lifetime of this Application.
    UltraCanvas::UltraCanvasApplicationBase& ultracanvas_app() { return *m_ultracanvas_app; }

private:
    Application();

    virtual void create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions&) override;
    virtual Core::EventLoop& create_platform_event_loop() override;

    // Non-owning: the instance is owned for the process lifetime by
    // UltraCanvasPlatform.cpp (which keeps this TU X11-free — see UltraCanvasPlatform.h).
    UltraCanvas::UltraCanvasApplicationBase* m_ultracanvas_app { nullptr };
};

}
