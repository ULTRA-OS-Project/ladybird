/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LibWebView side of the autocomplete bridge (see Autocomplete.h). This TU includes LibWebView
// (namespace GC) and so must stay X11-free — it never includes UltraCanvas window/app headers.

#include <AK/String.h>
#include <LibWebView/Autocomplete.h>

#include <UI/UltraCanvas/Autocomplete.h>

namespace Ladybird {

// Intentionally leaked (raw new, never deleted): like the bookmark/download observers, the engine
// must not be torn down at static-destruction time (its dtor would touch the already-destroyed
// Application / AutocompleteService). Recreated when the private/non-private mode changes.
static WebView::Autocomplete* s_autocomplete = nullptr;
static bool s_is_private = false;
static WebView::AutocompleteQueryID s_next_query_id = 1;
static std::function<void(std::string, std::vector<std::pair<std::string, std::string>>)> s_on_results;
static std::string s_current_query;

static std::string to_std(StringView view)
{
    return std::string { view.characters_without_null_termination(), view.length() };
}

static void ensure_engine(bool is_private)
{
    if (s_autocomplete && s_is_private == is_private)
        return;
    if (s_autocomplete)
        s_autocomplete->cancel_pending_query();

    s_autocomplete = new WebView::Autocomplete(is_private ? WebView::IsPrivate::Yes : WebView::IsPrivate::No);
    s_is_private = is_private;
    s_autocomplete->on_autocomplete_query_complete = [](WebView::AutocompleteQueryID, Vector<WebView::AutocompleteSuggestion> suggestions, WebView::AutocompleteResultKind) {
        std::vector<std::pair<std::string, std::string>> results;
        results.reserve(suggestions.size());
        for (auto const& suggestion : suggestions) {
            // `text` is the navigation value (URL or search term); the display text may add context.
            auto display = WebView::autocomplete_suggestion_display_text(suggestion);
            results.emplace_back(to_std(display.bytes_as_string_view()), to_std(suggestion.text.bytes_as_string_view()));
        }
        if (s_on_results)
            s_on_results(s_current_query, std::move(results));
    };
}

void request_autocomplete(std::string query, bool is_private,
    std::function<void(std::string, std::vector<std::pair<std::string, std::string>>)> on_results)
{
    ensure_engine(is_private);
    s_on_results = std::move(on_results);
    s_current_query = query;

    auto ak_query = String::from_utf8(StringView { query.data(), query.length() });
    if (ak_query.is_error())
        return;
    s_autocomplete->query_autocomplete_engine(s_next_query_id++, ak_query.release_value());
}

void cancel_autocomplete()
{
    if (s_autocomplete)
        s_autocomplete->cancel_pending_query();
}

}
