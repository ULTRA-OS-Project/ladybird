/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Types.h>
#include <LibCore/EventLoopImplementation.h>

// Forward-declared so this header stays free of the UltraCanvas headers (and the
// <X11/Xlib.h> macros they drag in). The bridge only uses the cross-platform base
// class API (StartTimer/AddFdWatch/PostToUIThread/Run/RunOnce/Exit).
namespace UltraCanvas {
class UltraCanvasApplicationBase;
}

namespace Ladybird {

class EventLoopImplementationUltraCanvas;

// Bridges Ladybird's Core::EventLoop onto the UltraCanvas toolkit's event loop, so
// the single UltraCanvas loop drives both the UI and all of Ladybird's IPC/timers.
// Mirrors the Qt (EventLoopManagerQt) and AppKit (EventLoopManagerMacOS) backends.
class EventLoopManagerUltraCanvas final : public Core::EventLoopManager {
public:
    explicit EventLoopManagerUltraCanvas(UltraCanvas::UltraCanvasApplicationBase&);
    virtual ~EventLoopManagerUltraCanvas() override;

    virtual NonnullOwnPtr<Core::EventLoopImplementation> make_implementation() override;

    virtual intptr_t register_timer(Core::EventReceiver&, int milliseconds, bool should_reload) override;
    virtual void unregister_timer(intptr_t timer_id) override;

    virtual void register_notifier(Core::Notifier&) override;
    virtual void unregister_notifier(Core::Notifier&) override;

    virtual void did_post_event() override;

    virtual int register_signal(int signal_number, Function<void(int)> handler) override;
    virtual void unregister_signal(int handler_id) override;

    UltraCanvas::UltraCanvasApplicationBase& app() { return m_app; }

private:
    UltraCanvas::UltraCanvasApplicationBase& m_app;

    // Maps each Core::Notifier to the UltraCanvas fd-watch id backing it, so
    // unregister_notifier can tear down the right watch. FdWatchId is u64.
    HashMap<Core::Notifier*, u64> m_notifier_watch_ids;
};

class EventLoopImplementationUltraCanvas final : public Core::EventLoopImplementation {
public:
    static NonnullOwnPtr<EventLoopImplementationUltraCanvas> create(EventLoopManagerUltraCanvas& manager)
    {
        return adopt_own(*new EventLoopImplementationUltraCanvas(manager));
    }

    virtual ~EventLoopImplementationUltraCanvas() override;

    virtual int exec() override;
    virtual size_t pump(PumpMode) override;
    virtual void quit(int) override;
    virtual void wake() override;
    virtual bool was_exit_requested() const override;

    // Called on the main event loop's implementation (see Application::
    // create_platform_event_loop) so exec() drives the real UltraCanvas Run(),
    // while nested Core::EventLoops pump one iteration at a time.
    void set_main_loop() { m_main_loop = true; }

private:
    explicit EventLoopImplementationUltraCanvas(EventLoopManagerUltraCanvas&);

    EventLoopManagerUltraCanvas& m_manager;
    bool m_main_loop { false };
    bool m_exit_requested { false };
    int m_exit_code { 0 };
};

}
