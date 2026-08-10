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
#include <LibGfx/Bitmap.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibURL/URL.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWebView/Application.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

#include <UI/UltraCanvas/UltraCanvasPlatform.h>
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

}

// A web-content view: an UltraCanvas element that blits the page's backing store
// and forwards input, and a WebView::ViewImplementation that owns the WebContent
// connection. Also implements the X11-free WebViewController the chrome drives.
class WebContentView final
    : public UltraCanvas::UltraCanvasUIElement
    , public WebView::ViewImplementation
    , public WebViewController {
public:
    WebContentView()
        : UltraCanvas::UltraCanvasUIElement("web-content-view")
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
        ViewImplementation::on_loading_state_change = [this](bool loading) {
            if (WebViewController::on_loading_state_change)
                WebViewController::on_loading_state_change(loading);
        };

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

        // Seed a non-zero viewport before initialize_client(): the base sends the
        // initial viewport and registers the compositor context using viewport_size(),
        // which would otherwise be 0x0 until the first layout pass — leaving WebContent
        // with a zero viewport and no backing stores to present. The real size follows
        // from the layout via handle_resize() in Render().
        m_viewport_size = { 1024, 768 };

        initialize_client(CreateNewClient::Yes);

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
    }

    virtual ~WebContentView() override = default;

    // ===== WebViewController =====
    virtual void load(StringView url) override
    {
        if (auto parsed = URL::create_with_url_or_path(url); parsed.has_value())
            ViewImplementation::load(parsed.release_value());
    }
    virtual void reload() override { ViewImplementation::reload(); }
    virtual void navigate_back() override { (void)traverse_the_history_by_delta(-1); }
    virtual void navigate_forward() override { (void)traverse_the_history_by_delta(1); }

    virtual void set_visible(bool visible) override
    {
        set_system_visibility_state(visible ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
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
        case UCEventType::KeyDown:
        case UCEventType::TextInput:
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
    void enqueue_mouse_event(Web::MouseEvent::Type type, UltraCanvas::UCEvent const& event)
    {
        Web::DevicePixelPoint position = { event.pointer.x, event.pointer.y };
        Web::DevicePixelPoint screen_position = { event.pointerGlobal.x, event.pointerGlobal.y };
        auto button = button_from(event.button);
        auto buttons = (type == Web::MouseEvent::Type::MouseDown) ? button : Web::UIEvents::MouseButton::None;

        if (button == Web::UIEvents::MouseButton::None
            && (type == Web::MouseEvent::Type::MouseDown || type == Web::MouseEvent::Type::MouseUp)) {
            return;
        }

        double wheel_delta_y = 0;
        if (type == Web::MouseEvent::Type::MouseWheel)
            wheel_delta_y = -static_cast<double>(event.wheelDelta);

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
};

WebViewHandle create_web_content_view()
{
    auto view = std::make_shared<WebContentView>();
    return WebViewHandle {
        std::static_pointer_cast<WebViewController>(view),
        std::static_pointer_cast<UltraCanvas::UltraCanvasUIElement>(view),
    };
}

}
