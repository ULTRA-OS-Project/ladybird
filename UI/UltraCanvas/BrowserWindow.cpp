/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// X11 side of the firewall: includes the UltraCanvas window/widget headers (which
// pull in <X11/Xlib.h>) but NEVER LibWebView. It drives each tab's web-content view
// only through the X11-free WebViewController interface.
#include <AK/String.h>

#include <UI/UltraCanvas/BrowserWindow.h>

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

static std::string to_std_string(String const& string)
{
    auto view = string.bytes_as_string_view();
    return std::string { view.characters_without_null_termination(), view.length() };
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
    };

    std::shared_ptr<UltraCanvas::UltraCanvasWindow> window;
    std::shared_ptr<UltraCanvas::UltraCanvasToolbar> toolbar;
    std::shared_ptr<UltraCanvas::UltraCanvasTextInput> address_bar;
    std::shared_ptr<UltraCanvas::UltraCanvasTabbedContainer> tab_container;
    std::vector<Tab> tabs;

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
    void relayout();
};

static std::vector<std::shared_ptr<BrowserWindowState>> s_windows;

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
    }

    auto index = tab_container->AddTab("New Tab", handle.element);
    Tab tab;
    tab.view = handle;
    tabs.push_back(tab);

    if (activate)
        tab_container->SetActiveTab(index);

    on_active_changed();
}

void BrowserWindowState::on_active_changed()
{
    auto active = active_index();
    int w = 0, h = 0;
    window->GetWindowSize(w, h);
    int content_width = w;
    int content_height = h - TOOLBAR_HEIGHT - TAB_STRIP_HEIGHT;

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
    int w = 0, h = 0;
    window->GetWindowSize(w, h);
    toolbar->SetElementSize(UltraCanvas::CSSLayout::Dimension::Px(static_cast<float>(w)),
        UltraCanvas::CSSLayout::Dimension::Px(static_cast<float>(TOOLBAR_HEIGHT)));
    tab_container->SetElementSize(UltraCanvas::CSSLayout::Dimension::Px(static_cast<float>(w)),
        UltraCanvas::CSSLayout::Dimension::Px(static_cast<float>(h - TOOLBAR_HEIGHT)));
    if (auto* controller = active_controller())
        controller->set_viewport_size(w, h - TOOLBAR_HEIGHT - TAB_STRIP_HEIGHT);
}

void open_browser_window(WebViewHandle const& first_view, StringView initial_url)
{
    auto state = std::make_shared<BrowserWindowState>();
    auto self = std::weak_ptr<BrowserWindowState>(state);

    UltraCanvas::WindowConfig config;
    config.title = "Ladybird";
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

    toolbar->AddButton("back", "Back", "", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->navigate_back(); });
    toolbar->AddButton("forward", "Forward", "", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->navigate_forward(); });
    toolbar->AddButton("reload", "Reload", "", [self] { if (auto s = self.lock()) if (auto* c = s->active_controller()) c->reload(); });

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

    // ===== Tabbed container: the tab strip + the active tab's web view.
    auto tab_container = std::make_shared<UltraCanvas::UltraCanvasTabbedContainer>("tabs", static_cast<float>(config.width), static_cast<float>(config.height - TOOLBAR_HEIGHT));
    tab_container->SetShowNewTabButton(true);
    state->tab_container = tab_container;

    tab_container->onTabChange = [self](int, int) { if (auto s = self.lock()) s->on_active_changed(); };
    tab_container->onNewTabRequest = [self] {
        auto s = self.lock();
        if (!s)
            return;
        s->add_tab(create_web_content_view(), true);
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

    window->AddChild(toolbar);
    window->AddChild(tab_container);

    window->onWindowResize = [self](int, int) { if (auto s = self.lock()) s->relayout(); };
    window->onWindowClosed = [self] {
        auto s = self.lock();
        if (!s)
            return;
        std::erase_if(s_windows, [&](auto const& e) { return e.get() == s.get(); });
    };

    // First tab uses the view created by the caller.
    state->add_tab(first_view, true);
    state->relayout();

    window->Show();
    s_windows.push_back(state);

    if (first_view.controller && !initial_url.is_empty())
        first_view.controller->load(initial_url);
}

}
