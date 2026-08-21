/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LibWebView side of the autocomplete bridge (see Autocomplete.h). This TU includes LibWebView
// (namespace GC) and so must stay X11-free — it never includes UltraCanvas window/app headers.

#include <AK/String.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebView/Autocomplete.h>

#include <UI/UltraCanvas/Autocomplete.h>

namespace Ladybird {

// Intentionally leaked (raw new, never deleted): like the bookmark/download observers, the engine
// must not be torn down at static-destruction time (its dtor would touch the already-destroyed
// Application / AutocompleteService). Recreated when the private/non-private mode changes.
static WebView::Autocomplete* s_autocomplete = nullptr;
static bool s_is_private = false;
static WebView::AutocompleteQueryID s_next_query_id = 1;
static std::function<void(std::string, std::vector<std::pair<std::string, std::string>>, std::string, int)> s_on_results;
static std::string s_current_query;

static std::string to_std(StringView view)
{
    return std::string { view.characters_without_null_termination(), view.length() };
}

// Chrome-style inline completion: if `suggestion_text` (as a URL) extends `query` in the form the
// user is typing, return {query + canonical suffix, byte offset where the suffix begins}. Mirrors the
// engine's autocomplete_url_can_complete() form-matching (full serialized / without scheme / without
// www — see Libraries/LibWebView/URL.cpp), but returns the completion instead of a bool.
static Optional<std::pair<std::string, int>> compute_inline_completion(StringView query, StringView suggestion_text)
{
    auto suggestion_url = URL::Parser::basic_parse(suggestion_text);
    if (query.is_empty() || !suggestion_url.has_value())
        return {};

    auto try_form = [&](StringView form) -> Optional<std::pair<std::string, int>> {
        auto check = form.ends_with('/') ? form.substring_view(0, form.length() - 1) : form;
        if (check.length() <= query.length() || !check.starts_with(query, CaseSensitivity::CaseInsensitive))
            return {};
        // Preserve the user's typed prefix casing; append the canonical suffix from the untrimmed form.
        std::string completed = to_std(query);
        completed += to_std(form.substring_view(query.length()));
        return std::make_pair(completed, static_cast<int>(query.length()));
    };

    auto serialized = suggestion_url->serialize(URL::ExcludeFragment::Yes);
    auto serialized_view = serialized.bytes_as_string_view();
    if (auto r = try_form(serialized_view); r.has_value())
        return r;

    if (!suggestion_url->scheme().is_one_of("http"sv, "https"sv))
        return {};

    auto scheme_end = serialized_view.find("://"sv);
    if (!scheme_end.has_value())
        return {};
    auto without_scheme = serialized_view.substring_view(*scheme_end + 3);
    if (auto r = try_form(without_scheme); r.has_value())
        return r;

    if (without_scheme.starts_with("www."sv, CaseSensitivity::CaseInsensitive)) {
        if (auto r = try_form(without_scheme.substring_view(4)); r.has_value())
            return r;
    }

    return {};
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

        // Inline-complete from the top suggestion when it prefix-extends the query (Chrome default).
        std::string completed_text;
        int selection_start = -1;
        if (!suggestions.is_empty()) {
            auto const& top = suggestions.first();
            if (top.can_be_automatically_selected && top.can_be_inline_completed
                && top.source != WebView::AutocompleteSuggestionSource::LiteralURL) {
                auto query_view = StringView { s_current_query.data(), s_current_query.length() };
                if (auto completion = compute_inline_completion(query_view, top.text.bytes_as_string_view()); completion.has_value()) {
                    completed_text = move(completion->first);
                    selection_start = completion->second;
                }
            }
        }

        if (s_on_results)
            s_on_results(s_current_query, std::move(results), std::move(completed_text), selection_start);
    };
}

void request_autocomplete(std::string query, bool is_private,
    std::function<void(std::string, std::vector<std::pair<std::string, std::string>>, std::string, int)> on_results)
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
