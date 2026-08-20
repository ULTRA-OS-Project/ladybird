/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// X11-free bridge for address-bar autocomplete (see Autocomplete.cpp for the LibWebView side).
// Mirrors the Bookmarks/Downloads firewall pattern: plain std types only, includable from the
// X11-side BrowserWindow.cpp. The underlying WebView::Autocomplete engine is asynchronous
// (history + bookmarks + remote search suggestions), so results arrive via the callback.

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Ladybird {

// Query the autocomplete engine for `query`. `is_private` selects a private engine (no history).
// When results are ready, on_results is invoked on the UI thread with the query it was for (so the
// caller can drop stale responses) and a list of {display text, navigation value} pairs.
void request_autocomplete(std::string query, bool is_private,
    std::function<void(std::string query, std::vector<std::pair<std::string, std::string>> results)> on_results);

// Cancel any in-flight query (e.g. when the address bar loses focus).
void cancel_autocomplete();

}
