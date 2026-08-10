/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// AK / LibCore first, so their headers are fully parsed before the UltraCanvas
// headers below pull in <X11/Xlib.h> (whose None/Bool/Status/... macros clash).
#include <AK/Vector.h>
#include <LibCore/Event.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibCore/ThreadEventQueue.h>

#include <UI/UltraCanvas/EventLoopImplementationUltraCanvas.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

// UltraCanvas headers last (they include X11).
#include <UltraCanvasApplication.h>

namespace Ladybird {

// ===== SIGNALS =====
// Async-signal-safe self-pipe: the OS handler writes the signal number to the
// pipe; an fd-watch on the read end dispatches to registered handlers on the UI
// thread. Mirrors the Qt backend's SignalHandlers, but delivered through the
// UltraCanvas fd-watch instead of a QSocketNotifier.
namespace {

int s_signal_pipe[2] = { -1, -1 };

struct SignalRegistration {
    int signal_number { 0 };
    int id { 0 };
    Function<void(int)> handler;
};

HashMap<int, Vector<SignalRegistration>> s_signal_handlers;
int s_next_signal_id = 1;

void os_signal_handler(int signal_number)
{
    // Runs in signal context: only async-signal-safe work (a single write()).
    if (s_signal_pipe[1] >= 0) {
        auto byte = static_cast<unsigned char>(signal_number);
        [[maybe_unused]] auto written = ::write(s_signal_pipe[1], &byte, 1);
    }
}

void dispatch_signal(int signal_number)
{
    auto it = s_signal_handlers.find(signal_number);
    if (it == s_signal_handlers.end())
        return;
    // Iterate in place: the handler Function is move-only so the vector cannot be
    // copied. Handlers unregistering themselves mid-dispatch is not expected here.
    for (auto& registration : it->value)
        registration.handler(signal_number);
}

}

EventLoopManagerUltraCanvas::EventLoopManagerUltraCanvas(UltraCanvas::UltraCanvasApplicationBase& app)
    : m_app(app)
{
}

EventLoopManagerUltraCanvas::~EventLoopManagerUltraCanvas() = default;

NonnullOwnPtr<Core::EventLoopImplementation> EventLoopManagerUltraCanvas::make_implementation()
{
    return EventLoopImplementationUltraCanvas::create(*this);
}

intptr_t EventLoopManagerUltraCanvas::register_timer(Core::EventReceiver& object, int milliseconds, bool should_reload)
{
    auto timer_id = m_app.StartTimer(static_cast<unsigned int>(milliseconds), should_reload,
        [weak_object = object.make_weak_ptr()](UltraCanvas::TimerId) {
            auto object = weak_object.strong_ref();
            if (!object)
                return;
            Core::TimerEvent event;
            object->dispatch_event(event);
        });
    return static_cast<intptr_t>(timer_id);
}

void EventLoopManagerUltraCanvas::unregister_timer(intptr_t timer_id)
{
    m_app.StopTimer(static_cast<UltraCanvas::TimerId>(timer_id));
}

void EventLoopManagerUltraCanvas::register_notifier(Core::Notifier& notifier)
{
    auto type = notifier.type() == Core::Notifier::Type::Write
        ? UltraCanvas::FdWatchType::Write
        : UltraCanvas::FdWatchType::Read;

    auto watch_id = m_app.AddFdWatch(notifier.fd(), type,
        [weak_notifier = notifier.make_weak_ptr()]() {
            if (!weak_notifier)
                return;
            Core::NotifierActivationEvent event;
            static_cast<Core::Notifier&>(*weak_notifier).dispatch_event(event);
        });

    m_notifier_watch_ids.set(&notifier, watch_id);
    notifier.set_owner_thread(pthread_self());
}

void EventLoopManagerUltraCanvas::unregister_notifier(Core::Notifier& notifier)
{
    if (auto watch_id = m_notifier_watch_ids.take(&notifier); watch_id.has_value())
        m_app.RemoveFdWatch(*watch_id);
}

void EventLoopManagerUltraCanvas::did_post_event()
{
    // A Core event was posted (possibly from another thread): wake the UltraCanvas
    // loop and drain the thread event queue on the UI thread.
    m_app.PostToUIThread([] {
        Core::ThreadEventQueue::current().process();
    });
}

int EventLoopManagerUltraCanvas::register_signal(int signal_number, Function<void(int)> handler)
{
    VERIFY(signal_number != 0);

    if (s_signal_pipe[0] < 0) {
        if (::pipe(s_signal_pipe) == 0) {
            for (int i = 0; i < 2; ++i) {
                auto flags = ::fcntl(s_signal_pipe[i], F_GETFL);
                ::fcntl(s_signal_pipe[i], F_SETFL, flags | O_NONBLOCK);
                ::fcntl(s_signal_pipe[i], F_SETFD, FD_CLOEXEC);
            }
            m_app.AddFdWatch(s_signal_pipe[0], UltraCanvas::FdWatchType::Read, [] {
                unsigned char buffer[64];
                ssize_t nread;
                while ((nread = ::read(s_signal_pipe[0], buffer, sizeof(buffer))) > 0) {
                    for (ssize_t i = 0; i < nread; ++i)
                        dispatch_signal(buffer[i]);
                }
            });
        }
    }

    int id = s_next_signal_id++;
    auto& handlers = s_signal_handlers.ensure(signal_number);
    bool is_first_for_signal = handlers.is_empty();
    handlers.append(SignalRegistration { signal_number, id, move(handler) });

    if (is_first_for_signal) {
        struct sigaction action = {};
        action.sa_handler = os_signal_handler;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        ::sigaction(signal_number, &action, nullptr);
    }
    return id;
}

void EventLoopManagerUltraCanvas::unregister_signal(int handler_id)
{
    VERIFY(handler_id != 0);
    for (auto& entry : s_signal_handlers) {
        auto& handlers = entry.value;
        auto size_before = handlers.size();
        handlers.remove_all_matching([&](auto& registration) { return registration.id == handler_id; });
        if (handlers.size() != size_before && handlers.is_empty()) {
            struct sigaction action = {};
            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);
            ::sigaction(entry.key, &action, nullptr);
        }
    }
}

EventLoopImplementationUltraCanvas::EventLoopImplementationUltraCanvas(EventLoopManagerUltraCanvas& manager)
    : m_manager(manager)
{
}

EventLoopImplementationUltraCanvas::~EventLoopImplementationUltraCanvas() = default;

int EventLoopImplementationUltraCanvas::exec()
{
    if (m_main_loop) {
        // Hand control to the UltraCanvas loop. Run() returns when the last window
        // closes or Exit() is called (see quit()). IPC/timers are serviced inside
        // each iteration via the notifier/timer bridges above.
        m_manager.app().Run();
        return m_exit_code;
    }

    // Nested Core::EventLoop (e.g. a synchronous IPC wait): pump until quit().
    while (!m_exit_requested)
        pump(PumpMode::WaitForEvents);
    return m_exit_code;
}

size_t EventLoopImplementationUltraCanvas::pump(PumpMode)
{
    auto processed = Core::ThreadEventQueue::current().process();
    m_manager.app().RunOnce();
    processed += Core::ThreadEventQueue::current().process();
    return processed;
}

void EventLoopImplementationUltraCanvas::quit(int code)
{
    m_exit_code = code;
    m_exit_requested = true;
    if (m_main_loop)
        m_manager.app().Exit();
    // Wake a blocked select() so the loop notices the exit request promptly.
    m_manager.app().PostToUIThread([] { });
}

void EventLoopImplementationUltraCanvas::wake()
{
    m_manager.app().PostToUIThread([] { });
}

bool EventLoopImplementationUltraCanvas::was_exit_requested() const
{
    if (m_main_loop)
        return m_exit_requested || !m_manager.app().IsRunning();
    return m_exit_requested;
}

}
