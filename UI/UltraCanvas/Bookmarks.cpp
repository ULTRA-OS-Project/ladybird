/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LibWebView side of the bookmark bridge (see Bookmarks.h). This TU includes LibWebView
// (namespace GC) and so must stay X11-free — it never includes UltraCanvas window/app headers.

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWebView/Application.h>
#include <LibWebView/BookmarkStore.h>
#include <LibWebView/Settings.h>

#include <UI/UltraCanvas/Bookmarks.h>

namespace Ladybird {

static std::function<void()> s_on_bookmarks_changed;
static std::function<void()> s_on_bookmarks_bar_visibility_changed;

static std::string to_std(StringView view)
{
    return std::string { view.characters_without_null_termination(), view.length() };
}

static StringView sv(std::string const& s)
{
    return StringView { s.data(), s.length() };
}

static String to_ak(std::string const& s)
{
    return MUST(String::from_utf8(sv(s)));
}

static Optional<String> to_optional_ak(std::string const& s)
{
    if (s.empty())
        return {};
    return to_ak(s);
}

static BookmarkNode to_node(WebView::BookmarkItem const& item)
{
    BookmarkNode node;
    node.id = to_std(item.id.bytes_as_string_view());
    if (item.is_folder()) {
        node.is_folder = true;
        auto const& folder = item.folder();
        if (folder.title.has_value())
            node.title = to_std(folder.title->bytes_as_string_view());
        for (auto const& child : folder.children)
            node.children.push_back(to_node(child));
    } else {
        auto const& bookmark = item.bookmark();
        auto url = bookmark.url.serialize();
        node.url = to_std(url.bytes_as_string_view());
        node.title = bookmark.title.has_value() ? to_std(bookmark.title->bytes_as_string_view()) : node.url;
        if (bookmark.favicon_base64_png.has_value())
            node.favicon_base64 = to_std(bookmark.favicon_base64_png->bytes_as_string_view());
    }
    return node;
}

std::vector<BookmarkNode> get_bookmarks()
{
    std::vector<BookmarkNode> result;
    for (auto const& item : WebView::Application::bookmark_store().root_items())
        result.push_back(to_node(item));
    return result;
}

void remove_bookmark(std::string const& id)
{
    WebView::Application::bookmark_store().remove_item(sv(id));
}

void edit_bookmark(std::string const& id, std::string const& url, std::string const& title)
{
    auto parsed = URL::create_with_url_or_path(sv(url));
    if (!parsed.has_value())
        return;
    WebView::Application::bookmark_store().edit_bookmark(sv(id), parsed.release_value(), to_optional_ak(title));
}

void edit_folder(std::string const& id, std::string const& title)
{
    WebView::Application::bookmark_store().edit_folder(sv(id), to_optional_ak(title));
}

void add_bookmarks_to_new_folder(std::string const& folder_title, std::vector<std::pair<std::string, std::string>> const& url_and_titles)
{
    auto& store = WebView::Application::bookmark_store();
    auto folder_id = store.add_folder(to_optional_ak(folder_title));
    for (auto const& [url, title] : url_and_titles) {
        auto parsed = URL::create_with_url_or_path(sv(url));
        if (!parsed.has_value())
            continue;
        store.add_bookmark(parsed.release_value(), to_optional_ak(title), {}, folder_id);
    }
}

void open_bookmark_in_new_tab(std::string const& id)
{
    WebView::Application::the().open_bookmark_in_new_tab(to_ak(id), Web::HTML::ActivateTab::Yes);
}

void open_bookmark_in_new_window(std::string const& id)
{
    WebView::Application::the().open_bookmark_in_new_window(to_ak(id), WebView::IsPrivate::No);
}

void open_bookmark_folder_in_new_tabs(std::string const& id)
{
    WebView::Application::the().open_bookmark_folder_in_new_tabs(to_ak(id));
}

bool bookmarks_bar_visible()
{
    return WebView::Application::settings().show_bookmarks_bar();
}

void set_bookmarks_bar_visible(bool visible)
{
    WebView::Application::settings().set_show_bookmarks_bar(visible);
}

namespace {

// Bridges the LibWebView observers to the X11-side callbacks. Both auto-register in their
// constructors, so they are created lazily once the store/settings exist.
class BridgeBookmarkObserver final : public WebView::BookmarkStoreObserver {
    virtual void bookmarks_changed() override
    {
        if (s_on_bookmarks_changed)
            s_on_bookmarks_changed();
    }
};

class BridgeSettingsObserver final : public WebView::SettingsObserver {
    virtual void show_bookmarks_bar_changed() override
    {
        if (s_on_bookmarks_bar_visibility_changed)
            s_on_bookmarks_bar_visibility_changed();
    }
};

// Intentionally leaked (raw new, never deleted): these observers must live for the whole process
// and must NOT be destroyed at static-destruction time. Their base dtors call remove_observer(),
// which dereferences Application::the() — but by static teardown the Application is already gone
// (main() returned), so owning them in an OwnPtr use-after-frees on exit (SIGSEGV). Leaking is
// harmless: the OS reclaims the memory, and the store/Settings drop their observer refs when the
// Application is destroyed.
static BridgeBookmarkObserver* s_bookmark_observer = nullptr;
static BridgeSettingsObserver* s_settings_observer = nullptr;

}

void set_on_bookmarks_changed(std::function<void()> callback)
{
    s_on_bookmarks_changed = move(callback);
    if (!s_bookmark_observer)
        s_bookmark_observer = new BridgeBookmarkObserver;
}

void set_on_bookmarks_bar_visibility_changed(std::function<void()> callback)
{
    s_on_bookmarks_bar_visibility_changed = move(callback);
    if (!s_settings_observer)
        s_settings_observer = new BridgeSettingsObserver;
}

}
