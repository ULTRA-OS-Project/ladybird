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
// When results are ready, on_results is invoked on the UI thread with:
//   - the query it was for (so the caller can drop stale responses),
//   - the ranked suggestions as {display text, navigation value} pairs,
//   - completed_text: the query inline-completed by the top prefix-extending suggestion (empty when
//     none applies), and selection_start: the byte offset in completed_text where the appended suffix
//     begins (-1 when there is no inline completion). The chrome shows completed_text with
//     [selection_start, end] selected so the next keystroke replaces it (Chrome-style).
void request_autocomplete(std::string query, bool is_private,
    std::function<void(std::string query, std::vector<std::pair<std::string, std::string>> results,
        std::string completed_text, int selection_start)> on_results);

// Cancel any in-flight query (e.g. when the address bar loses focus).
void cancel_autocomplete();

}
