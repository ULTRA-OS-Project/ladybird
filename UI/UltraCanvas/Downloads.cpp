/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// LibWebView side of the downloads bridge (see Downloads.h). This TU includes LibWebView
// (namespace GC) and so must stay X11-free — it never includes UltraCanvas window/app headers.

#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>

#include <UI/UltraCanvas/Downloads.h>

namespace Ladybird {

static std::function<void()> s_on_downloads_changed;

int active_download_count()
{
    int count = 0;
    for (auto const& download : WebView::Application::the().file_downloader().downloads()) {
        if (WebView::FileDownloader::status_is_active(download.status))
            ++count;
    }
    return count;
}

namespace {

// Bridges the LibWebView download observer to the X11-side callback. The base ctor auto-registers.
class BridgeDownloadObserver final : public WebView::FileDownloaderObserver {
    virtual void download_added(WebView::FileDownloader::Download const&) override { notify(); }
    virtual void download_updated(WebView::FileDownloader::Download const&) override { notify(); }
    virtual void download_removed(u64) override { notify(); }

    static void notify()
    {
        if (s_on_downloads_changed)
            s_on_downloads_changed();
    }
};

// Intentionally leaked (raw new, never deleted): like the bookmark observers, this must live for
// the whole process and must NOT be torn down at static-destruction time — its base dtor calls
// remove_observer(), which dereferences the already-destroyed Application (use-after-free / SIGSEGV
// on exit). Leaking is harmless; the FileDownloader drops its observer ref when destroyed.
static BridgeDownloadObserver* s_download_observer = nullptr;

}

void set_on_downloads_changed(std::function<void()> callback)
{
    s_on_downloads_changed = move(callback);
    if (!s_download_observer)
        s_download_observer = new BridgeDownloadObserver;
}

}
