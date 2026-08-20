/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

// X11 firewall.
//
// UltraCanvas's window/application headers pull in <X11/Xlib.h>, whose global-scope
// typedefs/macros (GC, None, Window, Status, Bool, ...) collide with Ladybird's
// identifiers (notably `namespace GC` from LibGC, reached via LibWebView). So X11
// headers and LibWebView headers must never share a translation unit.
//
// This header is deliberately X11-free AND LibWebView-free: it exposes the few
// window/application operations the LibWebView-side code needs as plain functions,
// implemented in UltraCanvasPlatform.cpp (which includes the X11-pulling headers but
// no LibWebView). The UltraCanvas UI-element/render/event headers are already X11-free
// and may be included directly alongside LibWebView.

namespace UltraCanvas {
class UltraCanvasApplicationBase;
}

namespace Ladybird {

// Create and initialize the UltraCanvas application (connects to X11, loads fonts).
// The instance is owned for the lifetime of the process; the returned reference is
// stored (non-owning) by Ladybird::Application.
UltraCanvas::UltraCanvasApplicationBase& create_and_initialize_ultracanvas_application(char const* app_name);

// Milestone 2 placeholder: open an empty top-level window so the UltraCanvas loop
// (driven via the Core::EventLoop bridge) keeps iterating. Replaced by BrowserWindow
// in Milestone 3.
void open_placeholder_window(char const* title, int width, int height);

// JavaScript dialogs (window.alert/confirm/prompt). These show an UltraCanvas modal
// dialog (X11) parented to the focused browser window and deliver the result via the
// callback. Strings are UTF-8; the callback runs on the UI thread when the dialog
// closes. Non-blocking (the dialog shows and these return immediately).
void show_alert_dialog(std::string const& message, std::function<void()> on_close);
void show_confirm_dialog(std::string const& message, std::function<void(bool accepted)> on_close);
void show_prompt_dialog(std::string const& message, std::string const& default_value,
    std::function<void(bool accepted, std::string const& text)> on_close);

// A toolkit-agnostic description of one context-menu entry. The LibWebView-side view
// builds a tree of these from a WebView::Menu (so no LibWebView types cross into the
// X11 side) and hands it to show_context_menu().
struct ContextMenuItem {
    std::string label;
    bool is_separator { false };
    bool enabled { true };
    bool checkable { false };
    bool checked { false };
    std::function<void()> on_activate;   // invoked on the UI thread when clicked
    std::vector<ContextMenuItem> submenu; // non-empty => this item opens a submenu
};

// Pop up a context menu at (window_x, window_y) — window-relative coordinates — on the
// focused browser window. Non-blocking; dismissed by click-outside or Escape.
void show_context_menu(std::vector<ContextMenuItem> items, int window_x, int window_y);

// Pop up an HTML <select> dropdown at (window_x, window_y). Like show_context_menu, but if the
// menu is dismissed WITHOUT choosing an item, on_dismiss is invoked exactly once (the engine
// needs a select_dropdown_closed({}) to unblock). Choosing an item runs its on_activate instead.
// minimum_width (logical px, the <select>'s width; 0 = auto) makes the popup at least that wide.
void show_select_dropdown(std::vector<ContextMenuItem> items, int window_x, int window_y, int minimum_width, std::function<void()> on_dismiss);

// Show a blocking native "Save As" dialog, seeded with initial_directory + suggested_name.
// Returns the chosen absolute path, or an empty string if the user cancelled (or no dialog
// could be shown). Blocks until dismissed — used for the synchronous download-path prompt.
std::string show_save_file_dialog(std::string const& title, std::string const& initial_directory, std::string const& suggested_name);

// Show a blocking native "Open File" dialog. Returns the chosen absolute path, or an empty
// string if the user cancelled (or no dialog could be shown). Used for Ctrl+O / "Open File…".
std::string show_open_file_dialog(std::string const& title, std::string const& initial_directory);

}
