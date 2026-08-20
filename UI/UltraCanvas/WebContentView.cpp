/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LibWebView side of the X11 firewall: this TU includes LibWebView (namespace GC)
// and the X11-FREE UltraCanvas element/render/event headers, but NEVER the
// UltraCanvas window/application headers (which pull <X11/Xlib.h>).

#include <AK/Utf8View.h>
#include <cstring>
#include <LibCore/Resource.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/URL.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/HTML/ColorPickerUpdateState.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWebView/Application.h>
#include <LibWebView/Menu.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

#include <UI/UltraCanvas/UltraCanvasPlatform.h>
#include <UI/UltraCanvas/Application.h>
#include <UI/UltraCanvas/WebViewController.h>

#include <cairo/cairo.h>

#include <UltraCanvasEvent.h>
#include <UltraCanvasRenderContext.h>
#include <UltraCanvasUIElement.h>

namespace Ladybird {

namespace {

Web::UIEvents::KeyModifier modifiers_from(UltraCanvas::UCEvent const& event)
{
    int mods = Web::UIEvents::Mod_None;
    if (event.ctrl)
        mods |= Web::UIEvents::Mod_Ctrl;
    if (event.shift)
        mods |= Web::UIEvents::Mod_Shift;
    if (event.alt)
        mods |= Web::UIEvents::Mod_Alt;
    if (event.meta)
        mods |= Web::UIEvents::Mod_Super;
    return static_cast<Web::UIEvents::KeyModifier>(mods);
}

Web::UIEvents::MouseButton button_from(UltraCanvas::UCMouseButton button)
{
    using UltraCanvas::UCMouseButton;
    switch (button) {
    case UCMouseButton::Left:
        return Web::UIEvents::MouseButton::Primary;
    case UCMouseButton::Right:
        return Web::UIEvents::MouseButton::Secondary;
    case UCMouseButton::Middle:
        return Web::UIEvents::MouseButton::Middle;
    case UCMouseButton::Back:
        return Web::UIEvents::MouseButton::Backward;
    case UCMouseButton::Forward:
        return Web::UIEvents::MouseButton::Forward;
    default:
        return Web::UIEvents::MouseButton::None;
    }
}

Web::UIEvents::KeyCode keycode_from(UltraCanvas::UCKeys key)
{
    using UltraCanvas::UCKeys;
    using namespace Web::UIEvents;
    switch (key) {
    case UCKeys::Return: // also UCKeys::Enter (same value)
        return Key_Return;
    case UCKeys::Tab:
        return Key_Tab;
    case UCKeys::Escape:
        return Key_Escape;
    case UCKeys::Space:
        return Key_Space;
    case UCKeys::Backspace:
        return Key_Backspace;
    case UCKeys::Delete:
        return Key_Delete;
    case UCKeys::Left:
        return Key_Left;
    case UCKeys::Right:
        return Key_Right;
    case UCKeys::Up:
        return Key_Up;
    case UCKeys::Down:
        return Key_Down;
    case UCKeys::Home:
        return Key_Home;
    case UCKeys::End:
        return Key_End;
    case UCKeys::PageUp:
        return Key_PageUp;
    case UCKeys::PageDown:
        return Key_PageDown;
    case UCKeys::Insert:
        return Key_Insert;
    default:
        break;
    }
    // Letters (0x41-0x5A) and digits (0x30-0x39) share values with Web::KeyCode.
    auto value = static_cast<int>(key);
    if ((value >= 0x30 && value <= 0x39) || (value >= 0x41 && value <= 0x5A))
        return static_cast<KeyCode>(value);
    return Key_Invalid;
}

u32 code_point_from(UltraCanvas::UCEvent const& event)
{
    if (!event.text.empty()) {
        Utf8View view { StringView { event.text.data(), event.text.length() } };
        if (!view.is_empty())
            return *view.begin();
    }
    if (event.character != 0)
        return static_cast<u32>(static_cast<unsigned char>(event.character));
    return 0;
}

UltraCanvas::UCMouseCursor cursor_from(Gfx::Cursor const& cursor)
{
    using UltraCanvas::UCMouseCursor;
    // Custom bitmap cursors (CSS url()) fall back to the arrow for now; UltraCanvas
    // supports file-backed cursors but WebContent hands us a shared bitmap, not a path.
    if (cursor.has<Gfx::ImageCursor>())
        return UCMouseCursor::Default;
    switch (cursor.get<Gfx::StandardCursor>()) {
    case Gfx::StandardCursor::Hidden:             return UCMouseCursor::NoCursor;
    case Gfx::StandardCursor::Crosshair:          return UCMouseCursor::Cross;
    case Gfx::StandardCursor::IBeam:              return UCMouseCursor::Text;
    case Gfx::StandardCursor::ResizeHorizontal:   return UCMouseCursor::SizeWE;
    case Gfx::StandardCursor::ResizeVertical:     return UCMouseCursor::SizeNS;
    case Gfx::StandardCursor::ResizeDiagonalTLBR: return UCMouseCursor::SizeNWSE;
    case Gfx::StandardCursor::ResizeDiagonalBLTR: return UCMouseCursor::SizeNESW;
    case Gfx::StandardCursor::ResizeColumn:       return UCMouseCursor::SizeWE;
    case Gfx::StandardCursor::ResizeRow:          return UCMouseCursor::SizeNS;
    case Gfx::StandardCursor::Hand:               return UCMouseCursor::Hand;
    case Gfx::StandardCursor::OpenHand:           return UCMouseCursor::Hand;
    case Gfx::StandardCursor::Help:               return UCMouseCursor::Help;
    case Gfx::StandardCursor::Drag:               return UCMouseCursor::SizeAll;
    case Gfx::StandardCursor::DragCopy:           return UCMouseCursor::SizeAll;
    case Gfx::StandardCursor::Move:               return UCMouseCursor::SizeAll;
    case Gfx::StandardCursor::Wait:               return UCMouseCursor::Wait;
    case Gfx::StandardCursor::Disallowed:         return UCMouseCursor::NotAllowed;
    case Gfx::StandardCursor::Zoom:               return UCMouseCursor::LookingGlass;
    case Gfx::StandardCursor::Eyedropper:         return UCMouseCursor::Cross;
    case Gfx::StandardCursor::None:
    case Gfx::StandardCursor::Arrow:
    default:                                      return UCMouseCursor::Default;
    }
}

std::string to_utf8_std(Utf16String const& string)
{
    auto utf8 = string.to_utf8();
    auto view = utf8.bytes_as_string_view();
    return std::string { view.characters_without_null_termination(), view.length() };
}

std::string sv_to_std(StringView view)
{
    return std::string { view.characters_without_null_termination(), view.length() };
}

// Translate a WebView::Menu (the engine's toolkit-agnostic context menu) into the
// firewall's ContextMenuItem tree so the X11 side never sees a LibWebView type. Each
// Action captures itself (ref-counted) so it stays alive until the menu is dismissed.
std::vector<ContextMenuItem> build_context_menu_items(WebView::Menu& menu)
{
    std::vector<ContextMenuItem> result;
    for (auto& menu_item : menu.items()) {
        menu_item.visit(
            [&](NonnullRefPtr<WebView::Action>& action) {
                if (!action->visible())
                    return;
                ContextMenuItem item;
                item.label = sv_to_std(action->text());
                item.enabled = action->enabled();
                item.checkable = action->is_checkable();
                item.checked = action->is_checkable() && action->checked();
                item.on_activate = [action] { action->activate(); };
                result.push_back(move(item));
            },
            [&](NonnullRefPtr<WebView::Menu>& submenu) {
                if (!submenu->visible())
                    return;
                ContextMenuItem item;
                item.label = sv_to_std(submenu->title());
                item.submenu = build_context_menu_items(*submenu);
                if (!item.submenu.empty())
                    result.push_back(move(item));
            },
            [&](WebView::Separator) {
                ContextMenuItem item;
                item.is_separator = true;
                result.push_back(move(item));
            });
    }
    return result;
}

}

// A web-content view: an UltraCanvas element that blits the page's backing store
// and forwards input, and a WebView::ViewImplementation that owns the WebContent
// connection. Also implements the X11-free WebViewController the chrome drives.
class WebContentView final
    : public UltraCanvas::UltraCanvasUIElement
    , public WebView::ViewImplementation
    , public WebViewController {
public:
    explicit WebContentView(bool is_private)
        // Constructing the ViewImplementation base as private makes initialize_client() below
        // launch a private WebContent client (private cookie jar, ephemeral storage, no history).
        : UltraCanvas::UltraCanvasUIElement("web-content-view")
        , WebView::ViewImplementation(is_private ? WebView::IsPrivate::Yes : WebView::IsPrivate::No)
    {
        // Explicit pixel size, kept in sync with the window by set_viewport_size().
        // (Vw/Vh percentages fed back through the layout and made the size oscillate.)
        SetElementSize(UltraCanvas::CSSLayout::Dimension::Px(1024), UltraCanvas::CSSLayout::Dimension::Px(768));

        // Repaint when WebContent presents a new frame. Fires from our IPC fd-watch;
        // RequestRedraw() marks this element dirty, and the next UpdateAndRender() (run
        // each event-loop iteration) blits the new front bitmap and composites to X11.
        on_ready_to_paint = [this]() { RequestRedraw(); };

        // Bridge ViewImplementation notifications to the chrome (X11-free strings).
        ViewImplementation::on_url_change = [this](URL::URL const& url) {
            if (WebViewController::on_url_change)
                WebViewController::on_url_change(url.serialize());
        };
        ViewImplementation::on_title_change = [this](Utf16String const& title) {
            if (WebViewController::on_title_change)
                WebViewController::on_title_change(title.to_utf8());
        };
        // set_favicon() populates favicon_base64_png() before firing this, so read it here and
        // forward the base64 PNG (or empty) to the chrome for the tab icon.
        ViewImplementation::on_favicon_change = [this](Optional<Gfx::Bitmap const&>) {
            if (WebViewController::on_favicon_change)
                WebViewController::on_favicon_change(favicon_base64_png().value_or(String {}));
        };
        ViewImplementation::on_loading_state_change = [this](bool loading) {
            if (WebViewController::on_loading_state_change)
                WebViewController::on_loading_state_change(loading);
        };
        // https→http fallback: when a scheme-less address was navigated as https:// (see load()),
        // the first finished network request is the main document. If it failed at the network
        // level (DNS/refused/TLS — not a normal 4xx/5xx, which carry no network_error), retry the
        // http:// version once. Acting on the first request only (release_value) avoids loops and
        // false-positives from later subresource failures. Only DevTools otherwise uses this hook.
        on_network_request_finished = [this](u64, u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error) {
            if (!m_scheme_probe_fallback.has_value())
                return;
            auto fallback = m_scheme_probe_fallback.release_value();
            if (network_error.has_value())
                load(fallback.bytes_as_string_view());
        };

        on_find_in_page = [this](size_t current_match_index, Optional<size_t> const& total_match_count) {
            if (WebViewController::on_find_result) {
                auto total = total_match_count.value_or(0);
                // The engine reports a 0-based index; present it 1-based, and 0 when empty.
                WebViewController::on_find_result(total == 0 ? 0 : current_match_index + 1, total);
            }
        };

        // Window manipulation (Fullscreen API, window.moveTo/resizeTo, minimize/maximize).
        // Forward to the chrome, which drives the top-level UltraCanvas window across the firewall.
        // (Qualify the left side: on_resize_window exists on both base classes.)
        ViewImplementation::on_fullscreen_window = [this] { if (WebViewController::on_enter_fullscreen) WebViewController::on_enter_fullscreen(); };
        ViewImplementation::on_exit_fullscreen_window = [this] { if (WebViewController::on_exit_fullscreen) WebViewController::on_exit_fullscreen(); };
        ViewImplementation::on_minimize_window = [this] { if (WebViewController::on_minimize) WebViewController::on_minimize(); };
        ViewImplementation::on_maximize_window = [this] { if (WebViewController::on_maximize) WebViewController::on_maximize(); };
        ViewImplementation::on_restore_window = [this] { if (WebViewController::on_restore) WebViewController::on_restore(); };
        ViewImplementation::on_reposition_window = [this](Gfx::IntPoint position) { if (WebViewController::on_move_window) WebViewController::on_move_window(position.x(), position.y()); };
        ViewImplementation::on_resize_window = [this](Gfx::IntSize size) { if (WebViewController::on_resize_window) WebViewController::on_resize_window(size.width(), size.height()); };

        // Hovered-link status display: forward the URL (empty on unhover) to the chrome.
        on_link_hover = [this](URL::URL const& url) { if (WebViewController::on_link_hover_change) WebViewController::on_link_hover_change(url.serialize()); };
        on_link_unhover = [this] { if (WebViewController::on_link_hover_change) WebViewController::on_link_hover_change(String {}); };

        // Reflect the page's hover cursor (pointer over links, I-beam over text, ...).
        // SetMouseCursor() is X11-free; the UltraCanvas app applies the hovered
        // element's cursor to the native window on the next pointer update.
        on_cursor_change = [this](Gfx::Cursor const& cursor) {
            SetMouseCursor(cursor_from(cursor));
        };

        // JavaScript dialogs (window.alert/confirm/prompt). Each shows an UltraCanvas
        // modal via the X11 firewall and reports the result back through the matching
        // *_closed() method so WebContent can resume the blocked script.
        on_request_alert = [this](Utf16String const& message) {
            show_alert_dialog(to_utf8_std(message), [this] { alert_closed(); });
        };
        on_request_confirm = [this](Utf16String const& message) {
            show_confirm_dialog(to_utf8_std(message), [this](bool accepted) { confirm_closed(accepted); });
        };
        on_request_prompt = [this](Utf16String const& message, Utf16String const& default_value) {
            show_prompt_dialog(to_utf8_std(message), to_utf8_std(default_value),
                [this](bool accepted, std::string const& text) {
                    if (accepted)
                        prompt_closed(Utf16String::from_utf8(StringView { text.data(), text.length() }));
                    else
                        prompt_closed({});
                });
        };

        // Context menus. WebContent pre-populates the page/link/image/media menus; on a
        // right-click it invokes Menu::on_activation, where we build a native popup from
        // the menu's (visible) actions and show it at the last pointer position.
        auto setup_context_menu = [this](WebView::Menu& menu, bool is_page_menu) {
            menu.on_activation = [this, menu = &menu, is_page_menu](Gfx::IntPoint) {
                // Standard-browser visibility for the page menu's edit items (Copy/Cut/Paste)
                // must be decided at click time, from the current selection/target.
                if (is_page_menu)
                    apply_standard_edit_visibility(*menu);
                show_context_menu(build_context_menu_items(*menu),
                    m_last_pointer_window.x(), m_last_pointer_window.y());
            };
        };
        setup_context_menu(page_context_menu(), true);
        setup_context_menu(link_context_menu(), false);
        setup_context_menu(selected_text_link_context_menu(), false);
        setup_context_menu(image_context_menu(), false);
        setup_context_menu(media_context_menu(), false);

        // HTML <select> dropdowns: the engine hands us the option list and blocks until we
        // report the chosen option id (or {} on dismiss) via select_dropdown_closed(). Show the
        // options as a native popup at the select's on-screen position.
        on_request_select_dropdown = [this](Gfx::IntPoint content_position, i32 minimum_width, Vector<Web::HTML::SelectItem> items) {
            std::vector<ContextMenuItem> menu_items;

            auto add_option = [&](Web::HTML::SelectItemOption const& option, bool in_group) {
                ContextMenuItem item;
                // Options are Action items (they close the popup on click, unlike checkboxes), so
                // mark the current selection with a checkmark and indent option-group members.
                std::string prefix = option.selected ? "\xe2\x9c\x94 " : "";
                if (in_group)
                    prefix = "    " + prefix;
                item.label = prefix + to_utf8_std(option.label);
                item.enabled = !option.disabled;
                auto id = option.id;
                item.on_activate = [this, id] { select_dropdown_closed(id); };
                menu_items.push_back(move(item));
            };

            for (auto const& select_item : items) {
                select_item.visit(
                    [&](Web::HTML::SelectItemOption const& option) { add_option(option, false); },
                    [&](Web::HTML::SelectItemOptionGroup const& group) {
                        ContextMenuItem header;
                        header.label = to_utf8_std(group.label);
                        header.enabled = false; // group label, not selectable
                        menu_items.push_back(move(header));
                        for (auto const& option : group.items)
                            add_option(option, true);
                    },
                    [&](Web::HTML::SelectItemSeparator const&) {
                        ContextMenuItem separator;
                        separator.is_separator = true;
                        menu_items.push_back(move(separator));
                    });
            }

            // content_position is relative to this view; offset by the view's window position.
            auto bounds = GetBoundsInWindow();
            int window_x = static_cast<int>(bounds.x) + content_position.x();
            int window_y = static_cast<int>(bounds.y) + content_position.y();
            show_select_dropdown(move(menu_items), window_x, window_y, minimum_width, [this] { select_dropdown_closed({}); });
        };

        // <input type=file>: show a native open dialog (single or multiple) and hand the chosen
        // files back to the engine. File filters are ignored for now (all files shown).
        on_request_file_picker = [this](auto const&, Web::HTML::AllowMultipleFiles allow_multiple) {
            Vector<Web::HTML::SelectedFile> selected_files;
            auto add_path = [&](std::string const& path) {
                if (path.empty())
                    return;
                if (auto file = WebView::create_selected_file(ByteString { path.data(), path.length() }); !file.is_error())
                    selected_files.append(file.release_value());
            };
            if (allow_multiple == Web::HTML::AllowMultipleFiles::Yes) {
                for (auto const& path : show_open_files_dialog("Select Files", std::string {}))
                    add_path(path);
            } else {
                add_path(show_open_file_dialog("Select File", std::string {}));
            }
            file_picker_closed(move(selected_files));
        };

        // <input type=color>: show a modal color picker seeded with the current value; report the
        // chosen color (or the unchanged one on cancel) back via color_picker_update(..., Closed).
        on_request_color_picker = [this](Gfx::Color current_color) {
            show_color_picker(current_color.red(), current_color.green(), current_color.blue(),
                [this](bool accepted, u8 r, u8 g, u8 b) {
                    color_picker_update(
                        accepted ? Optional<Gfx::Color>(Gfx::Color(r, g, b)) : Optional<Gfx::Color> {},
                        Web::HTML::ColorPickerUpdateState::Closed);
                });
        };

        // Seed a non-zero viewport before initialize_client(): the base sends the
        // initial viewport and registers the compositor context using viewport_size(),
        // which would otherwise be 0x0 until the first layout pass — leaving WebContent
        // with a zero viewport and no backing stores to present. The real size follows
        // from the layout via handle_resize() in Render().
        m_viewport_size = { 1024, 768 };

        initialize_client(CreateNewClient::Yes);

        // Send WebContent the system theme/palette. Without it the palette is empty, so the
        // default text-selection highlight color (palette.selection()) is transparent and
        // selections are invisible except on pages that define their own ::selection CSS.
        // (Qt/AppKit do this via update_palette() in their initialize_client override.)
        send_system_theme();

        // Give selected text a readable, desktop-style highlight: solid blue background with
        // white text. The engine's default selection only sets a (dark, themed) background and
        // leaves the text its original color, so black text on the dark highlight is hard to
        // read. A user-origin ::selection rule fixes the default while still letting a page
        // override it with its own ::selection styling (user origin loses to author origin).
        set_user_style_sheet("::selection { background-color: #3584e4; color: #ffffff; }"_string);

        // This backend presents CPU/shareable bitmaps (it blits front_bitmap through
        // Cairo) and has no GPU/DMABuf present path. Tell the Compositor the client
        // cannot present via GPU, so it delivers frames as CPU bitmaps
        // (did_present_bitmap -> server_did_paint -> has_usable_bitmap). Without this
        // the Compositor presents via GPU images we never consume, and the view stays
        // blank. Idempotent (guarded inside the Application).
        WebView::Application::the().notify_compositor_gpu_presentation_unavailable();

        // Give the Compositor a display + refresh rate. Presents are driven by a
        // per-display vsync scheduler keyed on the context's display_refresh_rate; with
        // no metadata the rate is 0, the vsync never ticks, and present_frame is never
        // called (backing stores are allocated but no frame is ever presented). Qt does
        // the same in its initialize_client().
        WebView::Application::the().update_compositor_display_metadata(
            client().compositor_context_id_for_page(m_client_state.page_index),
            m_display_id, m_maximum_frames_per_second);

        // Mark the page visible. Otherwise WebContent treats it as hidden (Page
        // Visibility API) and throttles/suspends rendering, so it never composites or
        // presents a frame. Qt does this in its showEvent().
        set_system_visibility_state(Web::HTML::VisibilityState::Visible);

        // A freshly-created view is the one the user is looking at.
        static_cast<Application&>(WebView::Application::the()).set_active_view(this);
    }

    virtual ~WebContentView() override
    {
        static_cast<Application&>(WebView::Application::the()).clear_active_view_if(this);
    }

    // ===== WebViewController =====
    virtual void load(StringView url) override
    {
        // sanitize_url turns user input into a URL like a real address bar: a bare host such as
        // "google.com" (no scheme) becomes "https://google.com", and non-URL text (e.g. a search
        // phrase) becomes a query on the configured search engine.
        auto sanitized = WebView::sanitize_url(url, WebView::Application::settings().search_engine());
        if (!sanitized.has_value())
            return;

        // If we guessed https:// for a scheme-less host, remember an http:// fallback so we can
        // retry once if the https navigation fails at the network level (some hosts are
        // http-only) — matching mainstream browsers. Cleared on any explicitly-schemed load. Only
        // arm it for a genuine host-guess (the sanitized URL is "https://<input>"), not a search
        // query that merely happens to be https.
        m_scheme_probe_fallback = {};
        auto trimmed = url.trim_whitespace();
        if (sanitized->scheme() == "https"sv && !trimmed.contains("://"sv)) {
            auto guessed_https = MUST(String::formatted("https://{}", trimmed));
            auto serialized = sanitized->serialize();
            if (serialized.bytes_as_string_view().starts_with(guessed_https.bytes_as_string_view()))
                m_scheme_probe_fallback = MUST(String::formatted("http://{}", trimmed));
        }

        ViewImplementation::load(sanitized.release_value());
    }
    virtual void reload() override { ViewImplementation::reload(); }
    virtual void navigate_back() override { (void)traverse_the_history_by_delta(-1); }
    virtual void navigate_forward() override { (void)traverse_the_history_by_delta(1); }
    virtual void zoom_in() override { ViewImplementation::zoom_in(); }
    virtual void zoom_out() override { ViewImplementation::zoom_out(); }
    virtual void reset_zoom() override { ViewImplementation::reset_zoom(); }
    virtual void set_zoom(double factor) override { ViewImplementation::set_zoom(factor); }
    virtual double current_zoom() const override { return zoom_level(); }
    virtual void start_find(StringView query, bool case_sensitive) override { ViewImplementation::find_in_page(Utf16String::from_utf8(query), case_sensitive ? CaseSensitivity::CaseSensitive : CaseSensitivity::CaseInsensitive); }
    virtual void find_next() override { find_in_page_next_match(); }
    virtual void find_previous() override { find_in_page_previous_match(); }
    virtual void stop_find() override { ViewImplementation::find_in_page(Utf16String {}); }
    virtual void toggle_bookmark() override { WebView::Application::the().toggle_bookmark_for_view(*this); }

    virtual void set_visible(bool visible) override
    {
        set_system_visibility_state(visible ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
        // The visible tab is the active one; report it so active_web_view()-based flows
        // (bookmarks, context menus) target this view.
        if (visible)
            static_cast<Application&>(WebView::Application::the()).set_active_view(this);
    }

    virtual void set_viewport_size(int width, int height) override
    {
        if (width <= 0 || height <= 0)
            return;
        SetElementSize(UltraCanvas::CSSLayout::Dimension::Px(static_cast<float>(width)),
            UltraCanvas::CSSLayout::Dimension::Px(static_cast<float>(height)));
        if (m_viewport_size == Gfx::IntSize { width, height })
            return;
        m_viewport_size = { width, height };
        // Mirror handle_resize() but signal resize-complete (::No) so the Compositor
        // presents instead of deferring forever (handle_resize always sends ::Yes).
        client().async_set_viewport(page_id(), viewport_size(), m_device_pixel_ratio, m_is_fullscreen);
        WebView::Application::the().update_compositor_viewport(
            client().compositor_context_id_for_page(page_id()),
            viewport_size().to_type<int>(),
            Web::Compositor::WindowResizingInProgress::No);
    }

    // ===== UltraCanvasUIElement =====
    virtual void Render(UltraCanvas::IRenderContext* ctx, UltraCanvas::Rect2Df const&) override
    {
        auto* cr = static_cast<cairo_t*>(ctx->GetNativeContext());
        if (!cr)
            return;

        // NOTE: the viewport is driven by set_viewport_size() from the window size, not
        // from GetWidth() here — reading the laid-out width per-frame fed back through
        // the layout and made the viewport grow without bound.

        Gfx::SharedImageBuffer const* image_buffer = nullptr;
        if (m_client_state.has_usable_bitmap)
            image_buffer = m_client_state.front_bitmap.shared_image_buffer.ptr();
        else if (m_backup_shared_image_buffer)
            image_buffer = m_backup_shared_image_buffer.ptr();

        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, GetWidth(), GetHeight());
        cairo_clip(cr);

        if (image_buffer) {
            auto bitmap = image_buffer->bitmap();
            // Gfx RGB32 and Cairo RGB24 share byte layout (BGRX) on little-endian,
            // so this wraps the shared scanlines with no per-pixel conversion. Cairo
            // requires the stride to equal cairo_format_stride_for_width(); if the Gfx
            // pitch differs the surface is created in an error state and paints nothing,
            // so fall back to a matched-stride copy in that case.
            auto width = bitmap->width();
            auto height = bitmap->height();
            auto pitch = static_cast<int>(bitmap->pitch());
            auto cairo_stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);

            cairo_surface_t* surface = nullptr;
            if (pitch == cairo_stride) {
                surface = cairo_image_surface_create_for_data(
                    bitmap->scanline_u8(0), CAIRO_FORMAT_RGB24, width, height, pitch);
            } else {
                surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
                if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
                    auto* dst = cairo_image_surface_get_data(surface);
                    auto dst_stride = cairo_image_surface_get_stride(surface);
                    auto row_bytes = min(pitch, dst_stride);
                    for (int y = 0; y < height; ++y)
                        memcpy(dst + static_cast<size_t>(y) * dst_stride, bitmap->scanline_u8(0) + static_cast<size_t>(y) * pitch, row_bytes);
                    cairo_surface_mark_dirty(surface);
                }
            }

            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_paint(cr);
            cairo_surface_destroy(surface);
        } else {
            auto color = page_background_color();
            cairo_set_source_rgb(cr, color.red() / 255.0, color.green() / 255.0, color.blue() / 255.0);
            cairo_paint(cr);
        }

        cairo_restore(cr);
    }

    virtual bool OnEvent(UltraCanvas::UCEvent const& event) override
    {
        using UltraCanvas::UCEventType;
        switch (event.type) {
        case UCEventType::MouseDown:
            SetFocus(true);
            enqueue_mouse_event(Web::MouseEvent::Type::MouseDown, event);
            return true;
        case UCEventType::MouseUp:
            enqueue_mouse_event(Web::MouseEvent::Type::MouseUp, event);
            return true;
        case UCEventType::MouseMove:
            enqueue_mouse_event(Web::MouseEvent::Type::MouseMove, event);
            return true;
        case UCEventType::MouseWheel:
            enqueue_mouse_event(Web::MouseEvent::Type::MouseWheel, event);
            return true;
        case UCEventType::TextInput: {
            // IME / composed text (e.g. CJK): a commit of more than one code point can't
            // be expressed as a single key event, so commit it directly through the
            // input-method path. Single code points fall through to the key-event path so
            // shortcuts and page keydown handlers still observe them.
            Utf8View text_view { StringView { event.text.data(), event.text.length() } };
            size_t code_points = 0;
            for (auto it = text_view.begin(); it != text_view.end() && code_points < 2; ++it)
                ++code_points;
            if (code_points > 1) {
                commit_text_from_input_method(Utf16String::from_utf8(StringView { event.text.data(), event.text.length() }));
                return true;
            }
            [[fallthrough]];
        }
        case UCEventType::KeyDown:
            enqueue_key_event(Web::KeyEvent::Type::KeyDown, event);
            return true;
        case UCEventType::KeyUp:
            enqueue_key_event(Web::KeyEvent::Type::KeyUp, event);
            return true;
        default:
            return UltraCanvas::UltraCanvasUIElement::OnEvent(event);
        }
    }

    virtual bool AcceptsFocus() const override { return true; }

    // ===== WebView::ViewImplementation =====
    virtual Web::DevicePixelSize viewport_size() const override
    {
        return m_viewport_size.to_type<Web::DevicePixels>();
    }
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override { return widget_position; }
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override { return content_position; }

private:
    // Load the bundled default theme and hand it to WebContent so the page palette (and thus
    // the default text-selection color) is populated. Best-effort: on failure the selection
    // simply stays as it was, so a missing resource never breaks page loading.
    void send_system_theme()
    {
        auto theme_ini = Core::Resource::load_from_uri("resource://themes/Default.ini"sv);
        if (theme_ini.is_error())
            return;
        auto theme = Gfx::load_system_theme(theme_ini.value()->filesystem_path().to_byte_string());
        if (theme.is_error())
            return;
        client().async_update_system_theme(m_client_state.page_index, theme.release_value());
    }

    // Standard-browser visibility for the page context menu's edit items:
    //  - Copy is shown only when text is selected (else there's nothing to copy);
    //  - Paste is shown only where Cut is — i.e. in an editable field. The engine already
    //    sets Cut's visibility from the right-click's input-events target (in
    //    did_request_page_context_menu, run just before this), so we mirror it onto Paste.
    // Cut itself needs no adjustment. build_context_menu_items() then honours visible().
    void apply_standard_edit_visibility(WebView::Menu& menu)
    {
        bool has_selection = !selected_text().is_empty();
        WebView::Action* cut = nullptr;
        WebView::Action* copy = nullptr;
        WebView::Action* paste = nullptr;
        for (auto& item : menu.items()) {
            item.visit(
                [&](NonnullRefPtr<WebView::Action>& action) {
                    switch (action->id()) {
                    case WebView::ActionID::CutSelection: cut = action.ptr(); break;
                    case WebView::ActionID::CopySelection: copy = action.ptr(); break;
                    case WebView::ActionID::Paste: paste = action.ptr(); break;
                    default: break;
                    }
                },
                [](auto&) {});
        }
        if (copy)
            copy->set_visible(has_selection);
        if (paste)
            paste->set_visible(cut ? cut->visible() : false);
    }

    void enqueue_mouse_event(Web::MouseEvent::Type type, UltraCanvas::UCEvent const& event)
    {
        // Remember where the pointer is (window-relative) so a context-menu request that
        // arrives asynchronously from WebContent can pop up at the click position.
        m_last_pointer_window = { event.pointerWindow.x, event.pointerWindow.y };

        Web::DevicePixelPoint position = { event.pointer.x, event.pointer.y };
        Web::DevicePixelPoint screen_position = { event.pointerGlobal.x, event.pointerGlobal.y };
        auto button = button_from(event.button);
        auto buttons = (type == Web::MouseEvent::Type::MouseDown) ? button : Web::UIEvents::MouseButton::None;

        if (button == Web::UIEvents::MouseButton::None
            && (type == Web::MouseEvent::Type::MouseDown || type == Web::MouseEvent::Type::MouseUp)) {
            return;
        }

        // UltraCanvas reports ±1 per wheel notch (X11 Button4/5), and WebContent
        // applies wheel_delta_y as device pixels — so a raw delta scrolls just 1px.
        // Scale by a per-notch step to get a Chrome-like speed (Qt's backend uses 40px).
        static constexpr double wheel_scroll_step_px = 40;
        double wheel_delta_y = 0;
        if (type == Web::MouseEvent::Type::MouseWheel)
            wheel_delta_y = -static_cast<double>(event.wheelDelta) * wheel_scroll_step_px;

        enqueue_input_event(Web::MouseEvent {
            type, position, screen_position, button, buttons, modifiers_from(event),
            0, wheel_delta_y, 1, nullptr });
    }

    void enqueue_key_event(Web::KeyEvent::Type type, UltraCanvas::UCEvent const& event)
    {
        auto keycode = keycode_from(event.virtualKey);
        auto modifiers = modifiers_from(event);
        auto code_point = code_point_from(event);

        if (event.virtualKey == UltraCanvas::UCKeys::Return || event.virtualKey == UltraCanvas::UCKeys::Enter)
            code_point = '\n';

        bool should_insert_text = type == Web::KeyEvent::Type::KeyDown && code_point != 0;
        enqueue_input_event(Web::KeyEvent { type, keycode, modifiers, code_point, false, should_insert_text, nullptr });
    }

    Gfx::IntSize m_viewport_size;
    Gfx::IntPoint m_last_pointer_window;
    // Set when load() guessed https:// for a scheme-less host; holds the http:// URL to retry
    // once if the https navigation fails at the network level. See the on_network_request_finished
    // handler and load().
    Optional<String> m_scheme_probe_fallback;
};

WebViewHandle create_web_content_view(bool is_private)
{
    auto view = std::make_shared<WebContentView>(is_private);
    return WebViewHandle {
        std::static_pointer_cast<WebViewController>(view),
        std::static_pointer_cast<UltraCanvas::UltraCanvasUIElement>(view),
    };
}

}
