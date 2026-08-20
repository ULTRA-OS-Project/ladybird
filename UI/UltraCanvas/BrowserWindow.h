/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// X11-free declaration (see UltraCanvasPlatform.h for the firewall rationale). The
// implementation in BrowserWindow.cpp is the X11 side and must not include LibWebView.
#include <AK/StringView.h>

#include <UI/UltraCanvas/WebViewController.h>

namespace Ladybird {

// Opens a top-level browser window hosting the given web-content view and navigates it to
// initial_url. When is_private, the window is a private-browsing window (shows a "Private" badge
// and its tabs use private views).
void open_browser_window(WebViewHandle const& view, StringView initial_url, bool is_private = false);

// Open `url` in a new tab of the currently-active browser window (or a new window if none
// is open). When `activate`, the new tab becomes the foreground tab. Used by the engine's
// "Open in new tab" / View Source flows, forwarded from Ladybird::Application across the
// X11 firewall. The new tab inherits the active window's private-browsing state.
void open_url_in_active_window(StringView url, bool activate);

// Open `url` in a brand-new browser window ("Open in new window" / "New (Private) Window").
void open_url_in_new_browser_window(StringView url, bool is_private = false);

}
