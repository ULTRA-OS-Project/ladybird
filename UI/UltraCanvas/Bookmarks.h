/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// X11-free bookmark bridge (see UltraCanvasPlatform.h for the firewall rationale).
//
// The bookmark DATA lives on the LibWebView side (WebView::Application::bookmark_store(),
// namespace GC), which cannot share a translation unit with the X11 headers. The bookmarks
// bar/menu UI is built on the X11 side (BrowserWindow.cpp). This header exposes the few
// bookmark operations the chrome needs as plain std functions/types; the implementation lives
// in Bookmarks.cpp (LibWebView side, no X11).

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Ladybird {

// A snapshot of one bookmark or folder in plain types, mirroring WebView::BookmarkItem.
struct BookmarkNode {
    std::string id;
    std::string title;
    std::string url;             // empty for folders
    std::string favicon_base64;  // base64 PNG (may carry a "data:...base64," prefix); empty if none
    bool is_folder { false };
    std::vector<BookmarkNode> children;
};

// The current bookmark tree (root items) — a cheap snapshot to rebuild the bar/menu from.
std::vector<BookmarkNode> get_bookmarks();

// Mutations (persisted; observers fire so the UI refreshes).
void remove_bookmark(std::string const& id);
void edit_bookmark(std::string const& id, std::string const& url, std::string const& title);
void edit_folder(std::string const& id, std::string const& title);
// Bookmark All Tabs: create a folder and add one bookmark per (url, title) pair.
void add_bookmarks_to_new_folder(std::string const& folder_title,
    std::vector<std::pair<std::string, std::string>> const& url_and_titles);

// Open a bookmark/folder (routes through the engine → chrome new-tab/window path).
void open_bookmark_in_new_tab(std::string const& id);
void open_bookmark_in_new_window(std::string const& id);
void open_bookmark_folder_in_new_tabs(std::string const& id);

// "Show bookmarks bar" setting (persisted to the profile config).
bool bookmarks_bar_visible();
void set_bookmarks_bar_visible(bool);

// Subscribe to changes; the callback runs on the UI thread. Registering also installs the
// underlying LibWebView observers on first use.
void set_on_bookmarks_changed(std::function<void()>);
void set_on_bookmarks_bar_visibility_changed(std::function<void()>);

}
