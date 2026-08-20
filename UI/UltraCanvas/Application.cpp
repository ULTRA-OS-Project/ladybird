/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// This TU includes LibWebView (via Application.h), so it must stay X11-free: it
// reaches the UltraCanvas application only through the firewall in
// UltraCanvasPlatform.h and the forward-declared base in the bridge header.
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventLoopImplementation.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibURL/URL.h>
#include <LibWeb/Clipboard/SystemClipboard.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/ViewImplementation.h>

#include <UI/UltraCanvas/Application.h>
#include <UI/UltraCanvas/BrowserWindow.h>
#include <UI/UltraCanvas/EventLoopImplementationUltraCanvas.h>
#include <UI/UltraCanvas/UltraCanvasPlatform.h>

// X11-free clipboard facade (the X11 backend is selected at link time). Safe to include
// alongside LibWebView — unlike the UltraCanvas window/app headers, it pulls no <X11/Xlib.h>.
#include <UltraCanvasClipboard.h>

namespace Ladybird {

static std::string to_std(StringView view)
{
    return std::string { view.characters_without_null_termination(), view.length() };
}

// Open a file or folder with the desktop's default handler (xdg-open). Best-effort;
// spawn failures are ignored (nothing to do if no handler is installed).
static void open_with_default_app(ByteString const& path)
{
    Vector<ByteString> arguments;
    arguments.append(path);
    Core::ProcessSpawnOptions options {
        .name = "xdg-open"sv,
        .executable = "xdg-open",
        .search_for_executable_in_path = true,
        .arguments = arguments,
    };
    (void)Core::Process::spawn(options);
}

Application::Application() = default;
Application::~Application() = default;

void Application::create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options)
{
    // This backend presents CPU/shareable bitmaps and has no GPU present path. Force
    // CPU painting so the Compositor uses a software Skia backend: with a GPU backend
    // its flush_async() completes only when the GPU signals, and that completion
    // callback is what actually sends did_present_frame to us — on machines where the
    // GPU/DMABuf path is unavailable it never fires, so no frame is ever presented and
    // the view stays blank. CPU painting makes the flush (and thus presentation)
    // synchronous and reliable.
    web_content_options.force_cpu_painting = WebView::ForceCPUPainting::Yes;

    // Milestone 4 will also wire the settings/config path here (cf. UI/Qt/Application.cpp).
}

Core::EventLoop& Application::create_platform_event_loop()
{
    if (!browser_options().headless_mode.has_value()) {
        // Create and initialize the UltraCanvas application (connects to X11, loads
        // fonts) before installing the event-loop manager, since the manager holds a
        // reference to it and the manager must be installed before the base creates
        // the main Core::EventLoop below. Creation is behind the X11 firewall.
        m_ultracanvas_app = &create_and_initialize_ultracanvas_application("Ladybird");
        Core::EventLoopManager::install(*new EventLoopManagerUltraCanvas(*m_ultracanvas_app));

        // Bring up the system clipboard now that the X11 display exists (the backend reads the
        // display from the application). Idempotent; if it fails, the clipboard overrides below
        // fall back to the in-memory base clipboard.
        (void)UltraCanvas::InitializeClipboard();
    }

    auto& event_loop = WebView::Application::create_platform_event_loop();

    if (!browser_options().headless_mode.has_value())
        static_cast<EventLoopImplementationUltraCanvas&>(event_loop.impl()).set_main_loop();

    return event_loop;
}

Optional<ByteString> Application::ask_user_for_download_path(ByteString const& file) const
{
    // "Download Linked File As…": prompt with a blocking native Save dialog, seeded with the
    // downloads directory and the suggested filename. An empty result means the user cancelled,
    // so return {} to cancel the download (path_for_downloaded_file maps that to ECANCELED).
    auto downloads_dir = Core::StandardPaths::downloads_directory();
    auto chosen = show_save_file_dialog("Save File",
        std::string { downloads_dir.characters(), downloads_dir.length() },
        std::string { file.characters(), file.length() });
    if (chosen.empty())
        return {};
    return ByteString { chosen.c_str(), chosen.length() };
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    auto folder = ByteString { path.dirname() };
    auto message = ByteString::formatted("Downloaded \"{}\".\nOpen the containing folder?", download_name);
    show_confirm_dialog(to_std(message), [folder](bool open_folder) {
        if (open_folder)
            open_with_default_app(folder);
    });
}

void Application::display_error_dialog(StringView error_message) const
{
    show_alert_dialog(to_std(error_message), [] { });
}

void Application::open_download(WebView::FileDownloader::Download const& download) const
{
    open_with_default_app(download.destination.string());
}

void Application::show_download_in_folder(WebView::FileDownloader::Download const& download) const
{
    open_with_default_app(ByteString { download.destination.dirname() });
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    if (m_active_view)
        return *m_active_view;
    return {};
}

NonnullRefPtr<Application::AddBookmarkPromise> Application::display_add_bookmark_dialog(Optional<String const&> target_folder_id) const
{
    // Ctrl+D: prompt for the bookmark name (pre-filled with the page title), then resolve with
    // the active page's URL + favicon. Rejecting on cancel leaves the page un-bookmarked.
    auto promise = AddBookmarkPromise::construct();
    auto view = active_web_view();
    if (!view.has_value()) {
        promise->reject(Error::from_string_literal("No active view to bookmark"));
        return promise;
    }

    auto url = view->url();
    auto favicon = view->favicon_base64_png();
    auto default_title = view->title().to_utf8();
    auto folder = target_folder_id.map([](auto& id) { return String { id }; });

    show_prompt_dialog("Bookmark name:", to_std(default_title.bytes_as_string_view()),
        [promise, url = move(url), favicon = move(favicon), folder = move(folder)](bool accepted, std::string const& text) mutable {
            if (!accepted) {
                promise->reject(Error::from_string_literal("Add bookmark cancelled"));
                return;
            }
            AddBookmarkDialogResult result;
            result.bookmark.url = move(url);
            result.bookmark.favicon_base64_png = move(favicon);
            if (!text.empty())
                result.bookmark.title = MUST(String::from_utf8(StringView { text.data(), text.length() }));
            result.target_folder_id = move(folder);
            promise->resolve(move(result));
        });

    return promise;
}

Utf16String Application::clipboard_text(ClipboardType type) const
{
    // Paste (and the web Clipboard API's readText) read here. Prefer the live system clipboard;
    // fall back to the in-memory base clipboard if it isn't available (headless / init failed).
    std::string text;
    if (UltraCanvas::GetClipboardText(text))
        return Utf16String::from_utf8(StringView { text.data(), text.length() });
    return WebView::Application::clipboard_text(type);
}

void Application::set_clipboard_text(String text, ClipboardType type)
{
    auto utf8 = text.to_byte_string();
    if (UltraCanvas::SetClipboardText(std::string { utf8.characters(), utf8.length() }))
        return;
    WebView::Application::set_clipboard_text(move(text), type);
}

void Application::insert_clipboard_item(Web::Clipboard::SystemClipboardItem item)
{
    // Copy, Cut and "Copy link address" all reach the clipboard through here (via the engine's
    // insert_clipboard_entry). Mirror any text/plain representation to the system clipboard so
    // it can be pasted into other applications, and keep the base's in-memory store so richer
    // representations (e.g. an image copy) still round-trip within the browser.
    for (auto const& representation : item.system_clipboard_representations) {
        if (representation.mime_type == "text/plain"sv) {
            UltraCanvas::SetClipboardText(std::string { representation.data.characters(), representation.data.length() });
            break;
        }
    }
    WebView::Application::insert_clipboard_item(move(item));
}

void Application::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab) const
{
    auto serialized = url.serialize();
    open_url_in_active_window(serialized.bytes_as_string_view(), activate_tab == Web::HTML::ActivateTab::Yes);
}

void Application::open_url_in_new_window(URL::URL const& url, WebView::IsPrivate is_private)
{
    auto serialized = url.serialize();
    open_url_in_new_browser_window(serialized.bytes_as_string_view(), is_private == WebView::IsPrivate::Yes);
}

Optional<WebView::ViewImplementation&> Application::open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const
{
    // Both callers (View Source, and the base open_url_in_new_tab) navigate the returned view
    // themselves right after, so create the tab WITHOUT loading anything — otherwise an
    // about:blank navigation would race and clobber their load_html()/load(). The new tab
    // activates on creation (WebContentView's ctor calls set_active_view), so it's the active
    // view.
    open_url_in_active_window(""sv, activate_tab == Web::HTML::ActivateTab::Yes);
    return active_web_view();
}

}
