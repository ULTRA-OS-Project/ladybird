/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Application.h>

// Forward-declared to keep this header free of the UltraCanvas (and X11) headers.
namespace UltraCanvas {
class UltraCanvasApplicationBase;
}

namespace Ladybird {

class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    virtual ~Application() override;

    // The UltraCanvas application instance that owns the toolkit event loop. Created
    // in create_platform_event_loop(); valid for the lifetime of this Application.
    UltraCanvas::UltraCanvasApplicationBase& ultracanvas_app() { return *m_ultracanvas_app; }

    // The chrome reports the active tab's view here (on tab switch / creation) so the
    // engine's active_web_view()-based flows (bookmarks, context menus) target it.
    void set_active_view(WebView::ViewImplementation* view) { m_active_view = view; }
    void clear_active_view_if(WebView::ViewImplementation const* view)
    {
        if (m_active_view == view)
            m_active_view = nullptr;
    }

private:
    Application();

    virtual void create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions&) override;
    virtual Core::EventLoop& create_platform_event_loop() override;

    // Downloads. ask_user_for_download_path is synchronous; the MVP auto-saves to the
    // default downloads directory. The rest show dialogs (via the X11 firewall) and open
    // files/folders via the system handler (xdg-open).
    virtual Optional<ByteString> ask_user_for_download_path(ByteString const& file) const override;
    virtual void display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const override;
    virtual void display_error_dialog(StringView error_message) const override;
    virtual void open_download(WebView::FileDownloader::Download const&) const override;
    virtual void show_download_in_folder(WebView::FileDownloader::Download const&) const override;

    // Bookmarks. active_web_view() returns the chrome's active tab; the add-bookmark
    // dialog auto-resolves with that page's URL+title (a silent "star", no prompt yet).
    virtual Optional<WebView::ViewImplementation&> active_web_view() const override;
    virtual NonnullRefPtr<AddBookmarkPromise> display_add_bookmark_dialog(Optional<String const&> target_folder_id = {}) const override;

    // Clipboard. Bridged to UltraCanvas's X11 clipboard (UltraCanvasClipboard.h is X11-free, so
    // this stays on the LibWebView side of the firewall). These are what make the context-menu
    // Copy / Cut / Paste / "Copy link address" items reach the real system clipboard; they fall
    // back to the base in-memory clipboard when the system one is unavailable (e.g. headless).
    virtual Utf16String clipboard_text(ClipboardType = ClipboardType::Text) const override;
    virtual void set_clipboard_text(String, ClipboardType = ClipboardType::Text) override;
    virtual void insert_clipboard_item(Web::Clipboard::SystemClipboardItem) override;

    // Open-in-new-tab / new-window (link + View Source context-menu actions). These forward the
    // request across the X11 firewall to the chrome (BrowserWindow.h, which is X11-free);
    // open_blank_new_tab returns the freshly-created active view so View Source can load into it.
    virtual void open_url_in_new_tab(URL::URL const&, Web::HTML::ActivateTab) const override;
    virtual void open_url_in_new_window(URL::URL const&, WebView::IsPrivate) override; // non-const in the base
    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab) const override;

    // Private browsing is supported (private windows use a private WebContent client). Enabling
    // this makes the engine's "Open in New Private Window" context-menu action visible.
    virtual bool supports_private_browsing_windows() const override { return true; }

    // Non-owning: the instance is owned for the process lifetime by
    // UltraCanvasPlatform.cpp (which keeps this TU X11-free — see UltraCanvasPlatform.h).
    UltraCanvas::UltraCanvasApplicationBase* m_ultracanvas_app { nullptr };
    WebView::ViewImplementation* m_active_view { nullptr };
};

}
