/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// X11-free bridge for the downloads toolbar button (see Downloads.cpp for the LibWebView side).
// Mirrors the Bookmarks.h firewall pattern: plain std types only, includable from the X11-side
// BrowserWindow.cpp.

#include <functional>

namespace Ladybird {

// Number of currently-active downloads (in-progress or paused).
int active_download_count();

// Register a callback fired whenever a download is added, updated or removed, so the chrome can
// refresh its downloads button/count. The backing observer is created lazily and lives for the
// process lifetime (see Downloads.cpp).
void set_on_downloads_changed(std::function<void()> callback);

}
