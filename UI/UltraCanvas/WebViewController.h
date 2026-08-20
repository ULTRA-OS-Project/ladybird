/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// X11-free AND LibWebView-free control surface for the web-content view.
//
// The view itself (WebContentView) inherits WebView::ViewImplementation and so
// pulls in LibWebView (namespace GC), which cannot share a TU with X11. The
// BrowserWindow lives on the X11 side, so it drives the view only through this
// interface, whose signatures use AK types (X11-safe, LibGC-free) and a
// forward-declared UltraCanvas element.

#include <AK/Function.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <memory>

namespace UltraCanvas {
class UltraCanvasUIElement;
}

namespace Ladybird {

// Actions the chrome invokes on the view, plus notifications the view sends back
// to the chrome (address bar / title / loading state).
class WebViewController {
public:
    virtual ~WebViewController() = default;

    virtual void load(StringView url) = 0;
    virtual void reload() = 0;
    virtual void navigate_back() = 0;
    virtual void navigate_forward() = 0;

    virtual void zoom_in() = 0;
    virtual void zoom_out() = 0;
    virtual void reset_zoom() = 0;
    // Set an exact zoom factor (1.0 = 100%; clamped by the engine to [0.3, 5.0]).
    virtual void set_zoom(double factor) = 0;
    // Current zoom factor of this view (1.0 = 100%).
    virtual double current_zoom() const = 0;

    // Find-in-page. start_find highlights all matches and jumps to the first;
    // find_next/find_previous cycle; stop_find clears the highlight.
    virtual void start_find(StringView query) = 0;
    virtual void find_next() = 0;
    virtual void find_previous() = 0;
    virtual void stop_find() = 0;

    // Bookmark (or un-bookmark) the current page.
    virtual void toggle_bookmark() = 0;

    // Set the on-screen size of the web view, in window (logical) pixels. Called by
    // the chrome from the authoritative X11 window size (initially and on resize) —
    // the view must NOT derive this from its own laid-out width, which feeds back
    // through the layout and oscillates.
    virtual void set_viewport_size(int width, int height) = 0;

    // Mark the tab visible/hidden. Background tabs are hidden so WebContent throttles
    // their rendering (Page Visibility API).
    virtual void set_visible(bool) = 0;

    Function<void(String)> on_url_change;
    Function<void(String)> on_title_change;
    // The page's favicon changed; the argument is a base64-encoded PNG (empty to clear).
    Function<void(String favicon_base64_png)> on_favicon_change;
    Function<void(bool loading)> on_loading_state_change;
    // Reports find results: 1-based index of the active match and the total count
    // (both 0 when there are no matches).
    Function<void(size_t current, size_t total)> on_find_result;
};

// The view exposed as both its control interface and its UltraCanvas element (for
// AddChild). Both share ownership of the single WebContentView instance.
struct WebViewHandle {
    std::shared_ptr<WebViewController> controller;
    std::shared_ptr<UltraCanvas::UltraCanvasUIElement> element;
};

// Creates a web-content view, spawning/attaching its WebContent process. When is_private is
// true the view uses a private WebContent client (private cookies, ephemeral storage, no history).
WebViewHandle create_web_content_view(bool is_private = false);

}
