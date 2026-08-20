/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// X11 side of the firewall: includes the UltraCanvas window/widget headers (which
// pull in <X11/Xlib.h>) but NEVER LibWebView. It drives each tab's web-content view
// only through the X11-free WebViewController interface.
#include <AK/Base64.h>
#include <AK/String.h>
#include <LibCore/Resource.h>

#include <UI/UltraCanvas/BrowserWindow.h>
#include <UI/UltraCanvas/Bookmarks.h>
#include <UI/UltraCanvas/UltraCanvasPlatform.h>

#include <UltraCanvasButton.h>
#include <UltraCanvasClipboard.h>
#include <UltraCanvasImage.h>
#include <UltraCanvasLabel.h>
#include <UltraCanvasMenu.h>
#include <UltraCanvasTabbedContainer.h>
#include <UltraCanvasTextInput.h>
#include <UltraCanvasToolbar.h>
#include <UltraCanvasWindow.h>

#include <memory>
#include <string>
#include <vector>

namespace Ladybird {

static constexpr int TOOLBAR_HEIGHT = 40;
static constexpr int TAB_STRIP_HEIGHT = 32; // UltraCanvasTabbedContainer default tabHeight
static constexpr int FIND_BAR_HEIGHT = 36;
static constexpr int BOOKMARKS_BAR_HEIGHT = 30;

static std::string to_std_string(String const& string)
{
    auto view = string.bytes_as_string_view();
    return std::string { view.characters_without_null_termination(), view.length() };
}

// Decode a bookmark's base64 PNG favicon into an in-memory UCImage for a button/menu icon.
// Returns null (text-only) when there's no favicon or it can't be decoded.
static std::shared_ptr<UltraCanvas::UCImage> decode_favicon(std::string const& favicon_base64)
{
    if (favicon_base64.empty())
        return nullptr;
    std::string payload = favicon_base64;
    // Strip an optional "data:image/png;base64," prefix.
    if (payload.rfind("data:", 0) == 0) {
        if (auto comma = payload.find(','); comma != std::string::npos)
            payload = payload.substr(comma + 1);
    }
    auto decoded = AK::decode_base64(StringView { payload.data(), payload.length() });
    if (decoded.is_error() || decoded.value().is_empty())
        return nullptr;
    return UltraCanvas::UCImageRaster::LoadFromMemory(decoded.value().data(), decoded.value().size());
}

// The default tab favicon (a globe glyph) shown for pages that provide none. Loaded once from
// the bundled resource (see UI/cmake/ResourceFiles.cmake); null if the resource is missing.
static std::shared_ptr<UltraCanvas::UCImage> default_favicon()
{
    static auto const globe = []() -> std::shared_ptr<UltraCanvas::UCImage> {
        auto resource = Core::Resource::load_from_uri("resource://icons/browser/globe.svg"sv);
        if (resource.is_error())
            return nullptr;
        return UltraCanvas::UCImageRaster::Get(to_std_string(resource.value()->filesystem_path()));
    }();
    return globe;
}

// The animated loading spinner (28-frame GIF) shown on a tab while its page loads. Loaded once
// from the bundled resource; null if missing (the tab then keeps the favicon/globe while loading).
static std::shared_ptr<UltraCanvas::UCImage> loading_gif()
{
    static auto const gif = []() -> std::shared_ptr<UltraCanvas::UCImage> {
        auto resource = Core::Resource::load_from_uri("resource://icons/browser/loading.gif"sv);
        if (resource.is_error())
            return nullptr;
        return UltraCanvas::UCImageRaster::Get(to_std_string(resource.value()->filesystem_path()));
    }();
    return gif;
}

// Resolve a bundled resource:// URI to a filesystem path and apply it as a toolbar
// button's (icon-only) glyph. Best-effort: on a missing resource the button simply
// stays as-is. The default mask/tint is kept so the SVG follows the toolbar's
// foreground color (unlike full-color favicons above, which disable masking).
static void set_toolbar_icon(std::shared_ptr<UltraCanvas::UltraCanvasButton> const& button, StringView uri)
{
    if (!button)
        return;
    auto resource = Core::Resource::load_from_uri(uri);
    if (resource.is_error())
        return;
    button->SetIcon(to_std_string(resource.value()->filesystem_path()));
    button->SetIconSize(20, 20);
    button->SetIconPosition(UltraCanvas::ButtonIconPosition::Center);
}

// Owns a browser window and all its tabs. Kept alive in s_windows for the window's
// lifetime and destroyed (with its tabs/views) inside the running event loop when the
// window closes — never at static teardown, which would use-after-free the IPC clients.
class BrowserWindowState : public std::enable_shared_from_this<BrowserWindowState> {
public:
    struct Tab {
        WebViewHandle view;
        std::string url;
        std::string title { "New Tab" };
        // The page's favicon (null => show the default globe). Kept so it can be restored when
        // loading finishes (the loading spinner overrides it while loading == true).
        std::shared_ptr<UltraCanvas::UCImage> favicon;
        bool loading { false };
    };

    std::shared_ptr<UltraCanvas::UltraCanvasWindow> window;
    std::shared_ptr<UltraCanvas::UltraCanvasToolbar> toolbar;
    std::shared_ptr<UltraCanvas::UltraCanvasTextInput> address_bar;
    std::shared_ptr<UltraCanvas::UltraCanvasTabbedContainer> tab_container;
    // Find-in-page bar (below the toolbar), hidden until Ctrl+F.
    std::shared_ptr<UltraCanvas::UltraCanvasToolbar> find_bar;
    std::shared_ptr<UltraCanvas::UltraCanvasTextInput> find_input;
    std::shared_ptr<UltraCanvas::UltraCanvasLabel> find_label;
    // Bookmarks bar (row of bookmark buttons, between the toolbar and find bar); visibility
    // follows the "Show bookmarks bar" setting.
    std::shared_ptr<UltraCanvas::UltraCanvasToolbar> bookmarks_bar;
    // The "hamburger" menu (Settings / Bookmarks / History / Downloads), kept alive while open.
    std::shared_ptr<UltraCanvas::UltraCanvasMenu> main_menu;
    // Popups spawned from the bookmarks bar (folder dropdown / right-click menu), kept alive
    // while shown.
    std::shared_ptr<UltraCanvas::UltraCanvasMenu> bookmark_folder_popup;
    std::shared_ptr<UltraCanvas::UltraCanvasMenu> bookmark_context_menu;
    std::vector<Tab> tabs;
    // Private-browsing window: its tabs use private views and the toolbar shows a "Private" badge.
    bool is_private { false };

    int active_index() const { return tab_container ? tab_container->GetActiveTab() : -1; }

    WebViewController* controller_at(int index)
    {
        if (index < 0 || index >= static_cast<int>(tabs.size()))
            return nullptr;
        return tabs[index].view.controller.get();
    }
    WebViewController* active_controller() { return controller_at(active_index()); }

    int index_of(WebViewController const* controller) const
    {
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i].view.controller.get() == controller)
                return static_cast<int>(i);
        }
        return -1;
    }

    void add_tab(WebViewHandle handle, bool activate);
    void on_active_changed();
    // Apply the correct tab icon for tabs[index]: the loading spinner while loading, otherwise
    // the page favicon (or the default globe).
    void apply_tab_icon(int index);
    void relayout();
    void close_active_tab();
    void switch_to_adjacent_tab(int delta); // +1 = next, -1 = previous (wraps)
    void open_file();                       // native open dialog -> load the chosen file
    void toggle_find_bar(bool show);
    void open_internal_page(char const* about_url);
    void show_main_menu();

    // Bookmarks.
    void rebuild_bookmarks_bar();
    void apply_bookmarks_bar_visibility();
    void bookmark_all_tabs();
    std::vector<UltraCanvas::MenuItemData> build_bookmark_menu_items(std::vector<BookmarkNode> const& nodes);
    void show_bookmark_folder_popup(BookmarkNode const& folder, int window_x, int window_y);
    void show_bookmark_context_menu(BookmarkNode const& node, int window_x, int window_y);
    void edit_bookmark_dialog(std::string id, std::string url, std::string title);
    void edit_folder_dialog(std::string id, std::string title);
};

static std::vector<std::shared_ptr<BrowserWindowState>> s_windows;

// The window most recently focused (or created). "Open in new tab" / View Source add their
// tab here; updated on window focus so a right-click in one of several windows targets it.
static std::weak_ptr<BrowserWindowState> s_active_window;

void BrowserWindowState::add_tab(WebViewHandle handle, bool activate)
{
    auto self = weak_from_this();
    auto* raw_controller = handle.controller.get();

    // Per-tab notifications: find this tab by its controller (indices shift as tabs
    // are added/removed), update its cached title/url + tab label, and reflect the
    // active tab's state into the window title and address bar.
    if (raw_controller) {
        raw_controller->on_title_change = [self, raw_controller](String title) {
            auto state = self.lock();
            if (!state)
                return;
            auto index = state->index_of(raw_controller);
            if (index < 0)
                return;
            state->tabs[index].title = title.is_empty() ? std::string { "New Tab" } : to_std_string(title);
            state->tab_container->SetTabTitle(index, state->tabs[index].title);
            if (index == state->active_index())
                state->window->SetWindowTitle(state->tabs[index].title);
        };
        raw_controller->on_url_change = [self, raw_controller](String url) {
            auto state = self.lock();
            if (!state)
                return;
            auto index = state->index_of(raw_controller);
            if (index < 0)
                return;
            state->tabs[index].url = to_std_string(url);
            if (index == state->active_index() && state->address_bar && !state->address_bar->IsFocused())
                state->address_bar->SetText(state->tabs[index].url);
        };
        raw_controller->on_favicon_change = [self, raw_controller](String favicon_base64) {
            auto state = self.lock();
            if (!state)
                return;
            auto index = state->index_of(raw_controller);
            if (index < 0)
                return;
            // Remember the favicon (null => default globe); apply_tab_icon shows it unless the tab
            // is still loading, in which case the spinner stays until the load finishes.
            state->tabs[index].favicon = decode_favicon(to_std_string(favicon_base64));
            state->apply_tab_icon(index);
        };
        raw_controller->on_loading_state_change = [self, raw_controller](bool loading) {
            auto state = self.lock();
            if (!state)
                return;
            auto index = state->index_of(raw_controller);
            if (index < 0)
                return;
            state->tabs[index].loading = loading;
            state->apply_tab_icon(index);
        };
        raw_controller->on_find_result = [self, raw_controller](size_t current, size_t total) {
            auto state = self.lock();
            if (!state || !state->find_label)
                return;
            // Only the active tab's results are shown in the (single) find bar.
            if (state->active_controller() != raw_controller)
                return;
            state->find_label->SetText(std::to_string(current) + "/" + std::to_string(total));
        };
    }

    auto index = tab_container->AddTab("New Tab", handle.element);
    Tab tab;
    tab.view = handle;
    tabs.push_back(tab);
    // Initial icon: default globe until the page reports a favicon or starts loading.
    apply_tab_icon(index);

    if (activate)
        tab_container->SetActiveTab(index);

    on_active_changed();
}

void BrowserWindowState::apply_tab_icon(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs.size()))
        return;
    auto& tab = tabs[index];
    if (tab.loading) {
        // Play the loading spinner (it overrides the static icon in the tab container).
        tab_container->SetTabIconAnimation(index, loading_gif());
    } else {
        tab_container->SetTabIconAnimation(index, nullptr);
        tab_container->SetTabIconImage(index, tab.favicon ? tab.favicon : default_favicon());
    }
}

void BrowserWindowState::on_active_changed()
{
    auto active = active_index();
    int w = 0, h = 0;
    window->GetWindowSize(w, h);
    int bm_h = (bookmarks_bar && bookmarks_bar->IsVisible()) ? BOOKMARKS_BAR_HEIGHT : 0;
    int find_h = (find_bar && find_bar->IsVisible()) ? FIND_BAR_HEIGHT : 0;
    int content_width = w;
    int content_height = h - TOOLBAR_HEIGHT - TAB_STRIP_HEIGHT - bm_h - find_h;

    for (size_t i = 0; i < tabs.size(); ++i) {
        bool is_active = static_cast<int>(i) == active;
        if (auto* controller = tabs[i].view.controller.get()) {
            controller->set_visible(is_active);
            if (is_active)
                controller->set_viewport_size(content_width, content_height);
        }
    }

    if (active >= 0 && active < static_cast<int>(tabs.size())) {
        window->SetWindowTitle(tabs[active].title.empty() ? std::string { "Ladybird" } : tabs[active].title);
        if (address_bar && !address_bar->IsFocused())
            address_bar->SetText(tabs[active].url);
    }
}

void BrowserWindowState::relayout()
{
    using UltraCanvas::CSSLayout::Dimension;
    int w = 0, h = 0;
    window->GetWindowSize(w, h);
    // The bookmarks bar and find bar only occupy space while visible; when hidden their height
    // collapses to 0 so the tab content fills the window.
    int bm_h = (bookmarks_bar && bookmarks_bar->IsVisible()) ? BOOKMARKS_BAR_HEIGHT : 0;
    int find_h = (find_bar && find_bar->IsVisible()) ? FIND_BAR_HEIGHT : 0;

    toolbar->SetElementSize(Dimension::Px(static_cast<float>(w)), Dimension::Px(static_cast<float>(TOOLBAR_HEIGHT)));
    if (bookmarks_bar)
        bookmarks_bar->SetElementSize(Dimension::Px(static_cast<float>(w)), Dimension::Px(static_cast<float>(bm_h)));
    if (find_bar)
        find_bar->SetElementSize(Dimension::Px(static_cast<float>(w)), Dimension::Px(static_cast<float>(find_h)));
    tab_container->SetElementSize(Dimension::Px(static_cast<float>(w)), Dimension::Px(static_cast<float>(h - TOOLBAR_HEIGHT - bm_h - find_h)));
    if (auto* controller = active_controller())
        controller->set_viewport_size(w, h - TOOLBAR_HEIGHT - TAB_STRIP_HEIGHT - bm_h - find_h);
}

void BrowserWindowState::toggle_find_bar(bool show)
{
    if (!find_bar)
        return;
    find_bar->SetVisible(show);
    relayout();
    if (show) {
        if (find_input) {
            window->SetFocusedElement(find_input.get());
            find_input->SelectAll();
            auto query = find_input->GetText();
            if (auto* controller = active_controller(); controller && !query.empty())
                controller->start_find(StringView { query.data(), query.length() });
        }
    } else {
        if (auto* controller = active_controller())
            controller->stop_find();
        if (find_label)
            find_label->SetText("");
    }
}

void BrowserWindowState::close_active_tab()
{
    // Closing the last tab closes the window (which exits the app when it's the last).
    if (tab_container->GetTabCount() <= 1) {
        window->Close();
        return;
    }
    int active = tab_container->GetActiveTab();
    if (active < 0 || active >= static_cast<int>(tabs.size()))
        return;
    // Drop our record first so indices stay aligned with the container when RemoveTab
    // fires onTabChange -> on_active_changed().
    tabs.erase(tabs.begin() + active);
    tab_container->RemoveTab(active);
}

void BrowserWindowState::switch_to_adjacent_tab(int delta)
{
    int count = tab_container->GetTabCount();
    if (count <= 1)
        return;
    int next = ((tab_container->GetActiveTab() + delta) % count + count) % count;
    tab_container->SetActiveTab(next);
    on_active_changed(); // ensure the newly-active view is shown/sized even if no onTabChange fires
}

void BrowserWindowState::open_file()
{
    // Native open dialog, then load the chosen file into the active tab. load()'s sanitize_url
    // turns an existing absolute path into a file:// URL.
    auto path = show_open_file_dialog("Open File", std::string {});
    if (path.empty())
        return;
    if (auto* controller = active_controller())
        controller->load(StringView { path.data(), path.length() });
}

void BrowserWindowState::open_internal_page(char const* about_url)
{
    // Settings/Bookmarks/History/Downloads are internal pages rendered by WebContent; open
    // each in a new tab, mirroring Ladybird's open_url_in_new_tab behaviour.
    add_tab(create_web_content_view(is_private), true);
    if (auto* controller = active_controller())
        controller->load(StringView { about_url, __builtin_strlen(about_url) });
}

void BrowserWindowState::show_main_menu()
{
    using UltraCanvas::MenuItemData;
    auto self = weak_from_this();

    if (!main_menu) {
        main_menu = std::make_shared<UltraCanvas::UltraCanvasMenu>("main-menu");
        main_menu->SetMenuType(UltraCanvas::MenuType::PopupMenu);
    }
    main_menu->Clear();
    main_menu->AddItem(MenuItemData::ActionWithShortcut("New Tab", "Ctrl+T", [self] {
        if (auto s = self.lock()) {
            s->add_tab(create_web_content_view(s->is_private), true);
            if (auto* c = s->active_controller())
                c->load("about:blank"sv);
        }
    }));
    main_menu->AddItem(MenuItemData::ActionWithShortcut("New Window", "Ctrl+N", [] { open_url_in_new_browser_window("about:blank"sv, false); }));
    main_menu->AddItem(MenuItemData::ActionWithShortcut("New Private Window", "Ctrl+Shift+N", [] { open_url_in_new_browser_window("about:blank"sv, true); }));
    main_menu->AddItem(MenuItemData::ActionWithShortcut("Open File...", "Ctrl+O", [self] { if (auto s = self.lock()) s->open_file(); }));
    main_menu->AddItem(MenuItemData::Separator());

    // Bookmarks submenu: Manage / Add / Bookmark All Tabs / Show Bar, then the bookmark list.
    std::vector<MenuItemData> bookmarks_submenu;
    bookmarks_submenu.push_back(MenuItemData::Action("Manage Bookmarks", [self] { if (auto s = self.lock()) s->open_internal_page("about:bookmarks"); }));
    bookmarks_submenu.push_back(MenuItemData::Separator());
    bookmarks_submenu.push_back(MenuItemData::ActionWithShortcut("Add Bookmark", "Ctrl+D", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->toggle_bookmark(); }));
    bookmarks_submenu.push_back(MenuItemData::ActionWithShortcut("Bookmark All Tabs...", "Ctrl+Shift+D", [self] { if (auto s = self.lock()) s->bookmark_all_tabs(); }));
    bookmarks_submenu.push_back(MenuItemData::Checkbox("Show Bookmarks Bar", bookmarks_bar_visible(), [](bool checked) { set_bookmarks_bar_visible(checked); }));
    bookmarks_submenu.push_back(MenuItemData::Separator());
    for (auto& item : build_bookmark_menu_items(get_bookmarks()))
        bookmarks_submenu.push_back(item);
    main_menu->AddItem(MenuItemData::Submenu("Bookmarks", bookmarks_submenu));

    main_menu->AddItem(MenuItemData::ActionWithShortcut("History", "Ctrl+H", [self] { if (auto s = self.lock()) s->open_internal_page("about:history"); }));
    main_menu->AddItem(MenuItemData::ActionWithShortcut("Downloads", "Ctrl+J", [self] { if (auto s = self.lock()) s->open_internal_page("about:downloads"); }));
    main_menu->AddItem(MenuItemData::Separator());

    // View submenu: tab switching + zoom controls + a zoom-percentage list.
    double zoom = 1.0;
    if (auto* c = active_controller())
        zoom = c->current_zoom();
    std::vector<MenuItemData> zoom_steps;
    static constexpr struct { char const* label; double factor; } STEPS[] = {
        { "300%", 3.0 }, { "250%", 2.5 }, { "200%", 2.0 }, { "175%", 1.75 }, { "150%", 1.5 },
        { "125%", 1.25 }, { "110%", 1.1 }, { "100%", 1.0 }, { "90%", 0.9 }, { "80%", 0.8 },
        { "75%", 0.75 }, { "67%", 0.67 }, { "50%", 0.5 }, { "33%", 0.33 }, { "25%", 0.25 }
    };
    for (auto const& step : STEPS) {
        std::string label = (zoom > step.factor - 0.005 && zoom < step.factor + 0.005)
            ? std::string { "\xe2\x9c\x94 " } + step.label
            : std::string { step.label };
        double factor = step.factor;
        zoom_steps.push_back(MenuItemData::Action(label, [self, factor] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->set_zoom(factor); }));
    }
    std::vector<MenuItemData> view_submenu;
    view_submenu.push_back(MenuItemData::ActionWithShortcut("Switch to Previous Tab", "Ctrl+PgUp", [self] { if (auto s = self.lock()) s->switch_to_adjacent_tab(-1); }));
    view_submenu.push_back(MenuItemData::ActionWithShortcut("Switch to Next Tab", "Ctrl+PgDown", [self] { if (auto s = self.lock()) s->switch_to_adjacent_tab(1); }));
    view_submenu.push_back(MenuItemData::Separator());
    view_submenu.push_back(MenuItemData::ActionWithShortcut("Zoom In", "Ctrl++", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->zoom_in(); }));
    view_submenu.push_back(MenuItemData::ActionWithShortcut("Zoom Out", "Ctrl+-", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->zoom_out(); }));
    view_submenu.push_back(MenuItemData::ActionWithShortcut("Zoom 100%", "Ctrl+0", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->reset_zoom(); }));
    view_submenu.push_back(MenuItemData::Submenu("Zoom", zoom_steps));
    main_menu->AddItem(MenuItemData::Submenu("View", view_submenu));

    main_menu->AddItem(MenuItemData::Separator());
    main_menu->AddItem(MenuItemData::Action("Settings", [self] { if (auto s = self.lock()) s->open_internal_page("about:settings"); }));

    int w = 0, h = 0;
    window->GetWindowSize(w, h);
    UltraCanvas::PopupElementSettings settings;
    settings.closeByEscapeKey = true;
    settings.closeByClickOutside = true;
    // Drop the menu just under the right end of the toolbar, where the ☰ button sits.
    main_menu->OpenMenu(UltraCanvas::Point2Di(w - 180, TOOLBAR_HEIGHT), *window, settings);
}

std::vector<UltraCanvas::MenuItemData> BrowserWindowState::build_bookmark_menu_items(std::vector<BookmarkNode> const& nodes)
{
    using UltraCanvas::MenuItemData;
    auto self = weak_from_this();
    std::vector<MenuItemData> items;
    for (auto const& node : nodes) {
        if (node.is_folder) {
            auto item = MenuItemData::Submenu(node.title.empty() ? "Folder" : node.title, build_bookmark_menu_items(node.children));
            items.push_back(item);
        } else {
            std::string url = node.url;
            auto item = MenuItemData::Action(node.title.empty() ? node.url : node.title, [self, url] {
                if (auto s = self.lock())
                    if (auto* c = s->active_controller())
                        c->load(StringView { url.data(), url.length() });
            });
            item.iconImage = decode_favicon(node.favicon_base64);
            item.tooltip = node.url;
            items.push_back(item);
        }
    }
    return items;
}

void BrowserWindowState::rebuild_bookmarks_bar()
{
    if (!bookmarks_bar)
        return;
    bookmarks_bar->ClearItems();
    auto self = weak_from_this();
    int index = 0;
    for (auto const& node : get_bookmarks()) {
        auto label = node.title.empty() ? (node.is_folder ? std::string { "Folder" } : node.url) : node.title;
        auto button = bookmarks_bar->AddButton("bm-" + std::to_string(index++), label, "", nullptr);
        if (!button)
            continue;
        auto* raw_button = button.get();
        button->SetOnClick([self, node, raw_button] {
            auto s = self.lock();
            if (!s)
                return;
            if (node.is_folder) {
                auto bounds = raw_button->GetBoundsInWindow();
                s->show_bookmark_folder_popup(node, static_cast<int>(bounds.x), static_cast<int>(bounds.y + bounds.height));
            } else if (auto* c = s->active_controller()) {
                c->load(StringView { node.url.data(), node.url.length() });
            }
        });
        button->onContextMenu = [self, node](int x, int y) {
            if (auto s = self.lock())
                s->show_bookmark_context_menu(node, x, y);
        };
        if (!node.is_folder) {
            if (auto icon = decode_favicon(node.favicon_base64)) {
                // Toolbar buttons default to drawing their icon as a monochrome mask (tinted
                // with the foreground color), which is right for themed UI glyphs but turns a
                // full-color favicon into a black silhouette. Draw favicons as real images.
                button->SetUseIconAsMask(false);
                button->SetIconSize(16, 16);
                button->SetIcon(icon);
            }
        }
    }
    relayout();
}

void BrowserWindowState::apply_bookmarks_bar_visibility()
{
    if (!bookmarks_bar)
        return;
    bool visible = bookmarks_bar_visible();
    bookmarks_bar->SetVisible(visible);
    if (visible)
        rebuild_bookmarks_bar();
    relayout();
}

void BrowserWindowState::show_bookmark_folder_popup(BookmarkNode const& folder, int window_x, int window_y)
{
    if (!bookmark_folder_popup) {
        bookmark_folder_popup = std::make_shared<UltraCanvas::UltraCanvasMenu>("bookmark-folder-popup");
        bookmark_folder_popup->SetMenuType(UltraCanvas::MenuType::PopupMenu);
    }
    bookmark_folder_popup->Clear();
    for (auto& item : build_bookmark_menu_items(folder.children))
        bookmark_folder_popup->AddItem(item);
    UltraCanvas::PopupElementSettings settings;
    settings.closeByEscapeKey = true;
    settings.closeByClickOutside = true;
    bookmark_folder_popup->OpenMenu(UltraCanvas::Point2Di(window_x, window_y), *window, settings);
}

void BrowserWindowState::show_bookmark_context_menu(BookmarkNode const& node, int window_x, int window_y)
{
    using UltraCanvas::MenuItemData;
    auto self = weak_from_this();
    if (!bookmark_context_menu) {
        bookmark_context_menu = std::make_shared<UltraCanvas::UltraCanvasMenu>("bookmark-context");
        bookmark_context_menu->SetMenuType(UltraCanvas::MenuType::PopupMenu);
    }
    bookmark_context_menu->Clear();

    std::string id = node.id;
    std::string url = node.url;
    std::string title = node.title;

    if (node.is_folder) {
        bookmark_context_menu->AddItem(MenuItemData::Action("Open All in Tabs", [id] { open_bookmark_folder_in_new_tabs(id); }));
        bookmark_context_menu->AddItem(MenuItemData::Separator());
        bookmark_context_menu->AddItem(MenuItemData::Action("Edit Folder...", [self, id, title] { if (auto s = self.lock()) s->edit_folder_dialog(id, title); }));
        bookmark_context_menu->AddItem(MenuItemData::Action("Delete Folder", [id] { remove_bookmark(id); }));
    } else {
        bookmark_context_menu->AddItem(MenuItemData::Action("Open in New Tab", [id] { open_bookmark_in_new_tab(id); }));
        bookmark_context_menu->AddItem(MenuItemData::Action("Open in New Window", [id] { open_bookmark_in_new_window(id); }));
        bookmark_context_menu->AddItem(MenuItemData::Separator());
        bookmark_context_menu->AddItem(MenuItemData::Action("Copy URL", [url] { UltraCanvas::SetClipboardText(url); }));
        bookmark_context_menu->AddItem(MenuItemData::Separator());
        bookmark_context_menu->AddItem(MenuItemData::Action("Edit Bookmark...", [self, id, url, title] { if (auto s = self.lock()) s->edit_bookmark_dialog(id, url, title); }));
        bookmark_context_menu->AddItem(MenuItemData::Action("Delete Bookmark", [id] { remove_bookmark(id); }));
    }

    UltraCanvas::PopupElementSettings settings;
    settings.closeByEscapeKey = true;
    settings.closeByClickOutside = true;
    bookmark_context_menu->OpenMenu(UltraCanvas::Point2Di(window_x, window_y), *window, settings);
}

void BrowserWindowState::edit_bookmark_dialog(std::string id, std::string url, std::string title)
{
    // Two sequential prompts (name, then URL); on both confirms, apply the edit.
    show_prompt_dialog("Bookmark name:", title, [id, url](bool accepted, std::string const& new_title) {
        if (!accepted)
            return;
        show_prompt_dialog("Bookmark URL:", url, [id, new_title](bool accepted2, std::string const& new_url) {
            if (!accepted2)
                return;
            edit_bookmark(id, new_url, new_title);
        });
    });
}

void BrowserWindowState::edit_folder_dialog(std::string id, std::string title)
{
    show_prompt_dialog("Folder name:", title, [id](bool accepted, std::string const& new_title) {
        if (!accepted)
            return;
        edit_folder(id, new_title);
    });
}

void BrowserWindowState::bookmark_all_tabs()
{
    std::vector<std::pair<std::string, std::string>> url_and_titles;
    for (auto const& tab : tabs) {
        if (!tab.url.empty())
            url_and_titles.emplace_back(tab.url, tab.title);
    }
    if (url_and_titles.empty())
        return;
    show_prompt_dialog("Save all tabs to folder:", "Saved Tabs", [url_and_titles](bool accepted, std::string const& folder_title) {
        if (!accepted)
            return;
        add_bookmarks_to_new_folder(folder_title, url_and_titles);
    });
}

void open_browser_window(WebViewHandle const& first_view, StringView initial_url, bool is_private)
{
    auto state = std::make_shared<BrowserWindowState>();
    state->is_private = is_private;
    auto self = std::weak_ptr<BrowserWindowState>(state);

    UltraCanvas::WindowConfig config;
    config.title = is_private ? "Ladybird (Private)" : "Ladybird";
    config.width = 1024;
    config.height = 768;

    auto window = UltraCanvas::CreateWindow(config);
    if (!window)
        return;
    state->window = window;

    // ===== Toolbar: back / forward / reload + address bar. Actions target the active tab.
    auto toolbar = std::make_shared<UltraCanvas::UltraCanvasToolbar>("chrome-toolbar", static_cast<float>(config.width), static_cast<float>(TOOLBAR_HEIGHT));
    toolbar->SetOrientation(UltraCanvas::ToolbarOrientation::Horizontal);
    state->toolbar = toolbar;

    auto back_button = toolbar->AddButton("back", "", "", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->navigate_back(); });
    set_toolbar_icon(back_button, "resource://icons/browser/arrow-left.svg"sv);
    auto forward_button = toolbar->AddButton("forward", "", "", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->navigate_forward(); });
    set_toolbar_icon(forward_button, "resource://icons/browser/arrow-right.svg"sv);
    auto reload_button = toolbar->AddButton("reload", "", "", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->reload(); });
    set_toolbar_icon(reload_button, "resource://icons/browser/rotate-cw.svg"sv);

    auto address_bar = toolbar->AddSearchBox("address-bar", "Enter address");
    state->address_bar = address_bar;
    if (address_bar) {
        address_bar->SetElementSize(UltraCanvas::CSSLayout::Dimension::Px(0), UltraCanvas::CSSLayout::Dimension::Px(28));
        address_bar->layoutItem.SetFlexGrow(1.0f);
        address_bar->onEnterPressed = [self](std::string const& text) -> bool {
            if (auto s = self.lock())
                if (auto* c = s->active_controller())
                    c->load(StringView { text.data(), text.length() });
            return true;
        };
    }

    // "Private" badge (right of the address bar), only in private-browsing windows.
    if (is_private)
        toolbar->AddLabel("private-badge", "Private");

    // Hamburger menu (Settings / Bookmarks / History / Downloads / New Tab), after the
    // address bar so it sits at the top-right.
    toolbar->AddButton("menu", "☰", "", [self] { if (auto s = self.lock()) s->show_main_menu(); });

    // ===== Tabbed container: the tab strip + the active tab's web view.
    auto tab_container = std::make_shared<UltraCanvas::UltraCanvasTabbedContainer>("tabs", static_cast<float>(config.width), static_cast<float>(config.height - TOOLBAR_HEIGHT));
    tab_container->SetShowNewTabButton(true);
    // Show a close (×) button on every tab; the onTabClose handler below removes it.
    tab_container->SetCloseMode(UltraCanvas::TabCloseMode::Closable);
    state->tab_container = tab_container;

    tab_container->onTabChange = [self](int, int) { if (auto s = self.lock()) s->on_active_changed(); };
    tab_container->onNewTabRequest = [self] {
        auto s = self.lock();
        if (!s)
            return;
        s->add_tab(create_web_content_view(s->is_private), true);
        if (auto* c = s->active_controller())
            c->load("about:blank"sv);
    };
    tab_container->onTabClose = [self](int index) -> bool {
        auto s = self.lock();
        if (!s)
            return true;
        // Closing the last tab closes the window.
        if (s->tabs.size() <= 1) {
            s->window->Close();
            return true;
        }
        if (index >= 0 && index < static_cast<int>(s->tabs.size()))
            s->tabs.erase(s->tabs.begin() + index);
        // The container removes its own tab entry (we returned true); refresh active state
        // on the next event loop turn via onTabChange.
        return true;
    };

    // ===== Find bar (bottom row): query box + prev/next + count + close. Hidden until Ctrl+F.
    auto find_bar = std::make_shared<UltraCanvas::UltraCanvasToolbar>("find-bar", static_cast<float>(config.width), static_cast<float>(FIND_BAR_HEIGHT));
    find_bar->SetOrientation(UltraCanvas::ToolbarOrientation::Horizontal);
    state->find_bar = find_bar;

    auto find_input = find_bar->AddSearchBox("find-input", "Find in page", [self](std::string const& text) {
        auto s = self.lock();
        if (!s)
            return;
        if (auto* c = s->active_controller()) {
            if (text.empty())
                c->stop_find();
            else
                c->start_find(StringView { text.data(), text.length() });
        }
    });
    state->find_input = find_input;
    if (find_input) {
        find_input->SetElementSize(UltraCanvas::CSSLayout::Dimension::Px(0), UltraCanvas::CSSLayout::Dimension::Px(24));
        find_input->layoutItem.SetFlexGrow(1.0f);
        find_input->onEnterPressed = [self](std::string const&) -> bool {
            if (auto s = self.lock())
                if (auto* c = s->active_controller())
                    c->find_next();
            return true;
        };
    }
    find_bar->AddButton("find-prev", "Prev", "", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->find_previous(); });
    find_bar->AddButton("find-next", "Next", "", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->find_next(); });
    state->find_label = find_bar->AddLabel("find-count", "");
    find_bar->AddButton("find-close", "Close", "", [self] { if (auto s = self.lock()) s->toggle_find_bar(false); });
    find_bar->SetVisible(false);

    // ===== Bookmarks bar (row of bookmark buttons). Populated from the store below; visibility
    // follows the "Show bookmarks bar" setting.
    auto bookmarks_bar = std::make_shared<UltraCanvas::UltraCanvasToolbar>("bookmarks-bar", static_cast<float>(config.width), static_cast<float>(BOOKMARKS_BAR_HEIGHT));
    bookmarks_bar->SetOrientation(UltraCanvas::ToolbarOrientation::Horizontal);
    state->bookmarks_bar = bookmarks_bar;

    // The container re-sorts children by z-index in Arrange() (UltraCanvasContainer::
    // SortChildrenByZOrder), and the vertical layout follows that order. With all z at 0
    // the sort is ambiguous, so set explicit z-indices to fix the stacking: main toolbar on
    // top, then the bookmarks bar, then the find bar, then the tab container fills the rest.
    toolbar->SetZIndex(0);
    bookmarks_bar->SetZIndex(1);
    find_bar->SetZIndex(2);
    tab_container->SetZIndex(3);
    window->AddChild(toolbar);
    window->AddChild(bookmarks_bar);
    window->AddChild(find_bar);
    window->AddChild(tab_container);

    window->onWindowResize = [self](int, int) { if (auto s = self.lock()) s->relayout(); };
    window->onWindowFocus = [self] { s_active_window = self; };
    window->onWindowClosed = [self] {
        auto s = self.lock();
        if (!s)
            return;
        std::erase_if(s_windows, [&](auto const& e) { return e.get() == s.get(); });
    };

    // Browser keyboard shortcuts. The window's event filter runs before the focused web
    // view (see UltraCanvasApplication::DispatchEventToElement), so these are handled
    // even while the page has keyboard focus; returning true consumes the key.
    window->InstallEventFilter(
        "browser-shortcuts",
        [self](UltraCanvas::UCEvent const& event) -> bool {
            if (event.type != UltraCanvas::UCEventType::KeyDown)
                return false;
            auto s = self.lock();
            if (!s)
                return false;
            // Escape closes the find bar when it's open (no modifier).
            if (event.virtualKey == UltraCanvas::UCKeys::Escape && s->find_bar && s->find_bar->IsVisible()) {
                s->toggle_find_bar(false);
                return true;
            }
            if (!event.ctrl)
                return false;
            // Ctrl+Shift shortcuts (bookmarks bar / bookmark all tabs / new private window).
            if (event.shift) {
                switch (event.virtualKey) {
                case UltraCanvas::UCKeys::B: // toggle the bookmarks bar
                    set_bookmarks_bar_visible(!bookmarks_bar_visible());
                    return true;
                case UltraCanvas::UCKeys::D: // bookmark all tabs
                    s->bookmark_all_tabs();
                    return true;
                case UltraCanvas::UCKeys::N: // new private window
                    open_url_in_new_browser_window("about:blank"sv, true);
                    return true;
                default:
                    return false;
                }
            }
            switch (event.virtualKey) {
            case UltraCanvas::UCKeys::T: // new tab
                s->add_tab(create_web_content_view(s->is_private), true);
                if (auto* c = s->active_controller())
                    c->load("about:blank"sv);
                return true;
            case UltraCanvas::UCKeys::N: // new window
                open_url_in_new_browser_window("about:blank"sv, false);
                return true;
            case UltraCanvas::UCKeys::W: // close tab
                s->close_active_tab();
                return true;
            case UltraCanvas::UCKeys::O: // open a local file
                s->open_file();
                return true;
            case UltraCanvas::UCKeys::J: // downloads page
                s->open_internal_page("about:downloads");
                return true;
            case UltraCanvas::UCKeys::H: // history page
                s->open_internal_page("about:history");
                return true;
            case UltraCanvas::UCKeys::PageUp: // previous tab
                s->switch_to_adjacent_tab(-1);
                return true;
            case UltraCanvas::UCKeys::PageDown: // next tab
                s->switch_to_adjacent_tab(1);
                return true;
            case UltraCanvas::UCKeys::L: // focus + select the address bar
                if (s->address_bar) {
                    s->window->SetFocusedElement(s->address_bar.get());
                    s->address_bar->SelectAll();
                }
                return true;
            case UltraCanvas::UCKeys::F: // find in page
                s->toggle_find_bar(true);
                return true;
            case UltraCanvas::UCKeys::D: // bookmark this page
                if (auto* c = s->active_controller())
                    c->toggle_bookmark();
                return true;
            case UltraCanvas::UCKeys::R: // reload
                if (auto* c = s->active_controller())
                    c->reload();
                return true;
            case UltraCanvas::UCKeys::Equal: // Ctrl+= / Ctrl++ : zoom in
            case UltraCanvas::UCKeys::Plus:
                if (auto* c = s->active_controller())
                    c->zoom_in();
                return true;
            case UltraCanvas::UCKeys::Minus: // zoom out
                if (auto* c = s->active_controller())
                    c->zoom_out();
                return true;
            case UltraCanvas::UCKeys::Key0: // reset zoom
                if (auto* c = s->active_controller())
                    c->reset_zoom();
                return true;
            default:
                return false;
            }
        },
        { UltraCanvas::UCEventType::KeyDown });

    // Refresh every window's bookmarks bar when the store changes, and show/hide every bar when
    // the setting toggles. Registered once for the process (the callbacks fan out to s_windows).
    static bool s_bookmark_callbacks_registered = false;
    if (!s_bookmark_callbacks_registered) {
        s_bookmark_callbacks_registered = true;
        set_on_bookmarks_changed([] {
            for (auto& w : s_windows)
                w->rebuild_bookmarks_bar();
        });
        set_on_bookmarks_bar_visibility_changed([] {
            for (auto& w : s_windows)
                w->apply_bookmarks_bar_visibility();
        });
    }

    // First tab uses the view created by the caller.
    state->add_tab(first_view, true);
    state->apply_bookmarks_bar_visibility(); // populate + show/hide per setting (also relayouts)
    state->relayout();

    window->Show();
    s_windows.push_back(state);
    // A newly-opened window is the one the user is looking at (onWindowFocus may not have
    // fired yet), so make it the target for subsequent open-in-new-tab requests.
    s_active_window = state;

    if (first_view.controller && !initial_url.is_empty())
        first_view.controller->load(initial_url);
}

void open_url_in_active_window(StringView url, bool activate)
{
    auto state = s_active_window.lock();
    if (!state) {
        // Every window has been closed; fall back to opening a fresh one.
        open_url_in_new_browser_window(url);
        return;
    }
    // New tabs inherit the active window's private-browsing state.
    auto handle = create_web_content_view(state->is_private);
    auto* controller = handle.controller.get();
    state->add_tab(handle, activate);
    // Load into the new tab specifically (not active_controller() — a background tab is not
    // the active one), so "Open in new tab" navigates the tab it just created.
    if (controller && !url.is_empty())
        controller->load(url);
}

void open_url_in_new_browser_window(StringView url, bool is_private)
{
    open_browser_window(create_web_content_view(is_private), url, is_private);
}

}
