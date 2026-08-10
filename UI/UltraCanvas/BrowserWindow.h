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

// Opens a top-level browser window hosting the given web-content view and navigates
// it to initial_url.
void open_browser_window(WebViewHandle const& view, StringView initial_url);

}
