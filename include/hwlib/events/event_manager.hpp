#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

namespace hwlib::events
{

/// One queued event: an identifier and the payload delivered with it.
template<typename Payload>
struct Event
{
    std::uint8_t id{};
    Payload payload{};
};

/// The queue the manager pushes into and dispatches from. Supplying it from the
/// outside is what keeps this component architecture-independent: the real
/// implementation is a platform primitive (an RTOS message queue), the tests use
/// a plain array. `TryPush` is the only operation reached from an interrupt, so
/// an implementation must make that one interrupt-safe and never blocking.
template<typename Queue, typename QueuedEvent>
concept EventQueueLike = requires(Queue& queue, const QueuedEvent& in, QueuedEvent& out) {
    {
        queue.TryPush(in)
    } -> std::same_as<bool>;
    {
        queue.TryPop(out)
    } -> std::same_as<bool>;
    {
        queue.PopBlocking(out)
    } -> std::same_as<bool>;
};

inline constexpr std::size_t DEFAULT_MAX_SUBSCRIPTIONS = 12U;

/// How often a jammed queue is allowed to complain. A full queue means the loop
/// draining it is behind, and a periodic producer keeps pushing at its own rate —
/// unthrottled, that turns a slow loop into tens of reports a second, and the cost
/// of emitting them (a UART or RTT log backend flushes) slows the loop further. The
/// drop is worth knowing about; each individual drop is not.
inline constexpr std::uint32_t QUEUE_FULL_REPORT_PERIOD_MS = 1000U;

/// Anything with lock() and unlock(), guarding the queue-full accounting. On a platform
/// where drops can come from several contexts at once — a thread and an interrupt — that
/// is its critical section, for example a wrapper around irq_lock().
template<typename Lock>
concept EventManagerLockable = requires(Lock& lock) {
    lock.lock();
    lock.unlock();
};

namespace detail
{

/// The default: no locking, for a manager pushed to from one context.
struct EventManagerNoLock
{
    constexpr void lock() noexcept {}

    constexpr void unlock() noexcept {}
};

template<EventManagerLockable Lock>
class EventManagerGuard
{
public:
    explicit EventManagerGuard(Lock& lock)
        : m_lock{lock}
    {
        m_lock.lock();
    }

    EventManagerGuard(const EventManagerGuard&)            = delete;
    EventManagerGuard& operator=(const EventManagerGuard&) = delete;
    EventManagerGuard(EventManagerGuard&&)                 = delete;
    EventManagerGuard& operator=(EventManagerGuard&&)      = delete;

    ~EventManagerGuard()
    {
        m_lock.unlock();
    }

private:
    Lock& m_lock;
};

} // namespace detail

/// Publish/subscribe over a queue: handlers registered per event id, events
/// dispatched on whatever context calls `Dispatch()` or `TryDispatch()`.
///
/// Time is passed in rather than read: `Push()` takes `nowMs` so the throttling of
/// the queue-full report is testable on the host with a fake clock, and so the
/// component needs no clock of its own.
///
/// A queue that copies an event as raw bytes — an RTOS message queue does —
/// additionally requires `Event<Payload>` to be trivially copyable. That is the
/// queue's contract, not this component's, so it is asserted by the implementation
/// that has it rather than here, where a host fake is free to hold a `std::string`.
template<typename Payload, EventQueueLike<Event<Payload>> Queue,
         std::size_t MAX_SUBSCRIPTIONS = DEFAULT_MAX_SUBSCRIPTIONS,
         EventManagerLockable Lock     = detail::EventManagerNoLock>
class EventManager
{
public:
    using EventId             = std::uint8_t;
    using Handler             = std::function<void(const Payload&)>;
    using OnSubscriptionsFull = std::function<void(EventId)>;
    using OnEventsDropped     = std::function<void(EventId, std::uint32_t)>;

    /// The queue is borrowed, not owned: it outlives the manager and usually lives
    /// in the platform adapter that also feeds the dispatch loop.
    explicit EventManager(Queue& queue)
        : m_queue{queue}
    {}

    EventManager(const EventManager&)            = delete;
    EventManager& operator=(const EventManager&) = delete;
    EventManager(EventManager&&)                 = delete;
    EventManager& operator=(EventManager&&)      = delete;
    ~EventManager()                              = default;

    /// Reports a dropped subscription instead of growing the table: the cap is a
    /// deliberate bound on static storage, and exceeding it is a configuration
    /// error the integrator has to see.
    void SetOnSubscriptionsFull(OnSubscriptionsFull onSubscriptionsFull)
    {
        m_onSubscriptionsFull = std::move(onSubscriptionsFull);
    }

    /// The lock guarding the drop accounting — for a lock that needs setting up, and
    /// for tests.
    [[nodiscard]] Lock& DropLock() noexcept
    {
        return m_lock;
    }

    /// Reports the number of events dropped since the previous report, at most once
    /// per QUEUE_FULL_REPORT_PERIOD_MS. Called from whatever context `Push()` runs
    /// on, including an interrupt.
    void SetOnEventsDropped(OnEventsDropped onEventsDropped)
    {
        m_onEventsDropped = std::move(onEventsDropped);
    }

    void Subscribe(EventId id, Handler handler)
    {
        if (m_count >= MAX_SUBSCRIPTIONS)
        {
            if (m_onSubscriptionsFull)
            {
                m_onSubscriptionsFull(id);
            }
            return;
        }
        m_subscriptions.at(m_count) = Subscription{id, std::move(handler)};
        ++m_count;
    }

    /// Enum-typed convenience overload — callers write `Subscribe(AppEvent::eTick, ...)`
    /// without a cast. The enumeration's underlying type is assumed to fit EventId.
    template<typename Enum>
        requires std::is_enum_v<Enum>
    void Subscribe(Enum id, Handler handler)
    {
        Subscribe(static_cast<EventId>(id), std::move(handler));
    }

    /// Queues an event. Never blocks, so it is safe to call from an interrupt.
    /// Returns false if the queue is full, in which case the event is dropped and
    /// counted towards the throttled drop report.
    [[nodiscard]] bool Push(EventId id, const Payload& payload, std::uint32_t nowMs)
    {
        const Event<Payload> event{id, payload};
        if (m_queue.TryPush(event))
        {
            return true;
        }
        ReportDrop(id, nowMs);
        return false;
    }

    template<typename Enum>
        requires std::is_enum_v<Enum>
    [[nodiscard]] bool Push(Enum id, const Payload& payload, std::uint32_t nowMs)
    {
        return Push(static_cast<EventId>(id), payload, nowMs);
    }

    /// Blocks until an event is available, then calls every handler registered for
    /// its id. Returns true if at least one handler matched.
    bool Dispatch()
    {
        Event<Payload> event{};
        if (!m_queue.PopBlocking(event))
        {
            return false;
        }
        return Deliver(event);
    }

    /// Non-blocking single-event dispatch, for callers that poll from an existing
    /// loop instead of dedicating a thread to `Dispatch()`. Returns true if an event
    /// was pulled off the queue — whether or not a handler matched it — so a loop
    /// drains with `while (TryDispatch()) {}`.
    bool TryDispatch()
    {
        Event<Payload> event{};
        if (!m_queue.TryPop(event))
        {
            return false;
        }
        std::ignore = Deliver(event);
        return true;
    }

private:
    struct Subscription
    {
        EventId id{};
        Handler handler;
    };

    bool Deliver(const Event<Payload>& event)
    {
        bool matched = false;
        for (std::size_t i = 0U; i < m_count; ++i)
        {
            const auto& subscription = m_subscriptions.at(i);
            if (subscription.id != event.id)
            {
                continue;
            }
            matched = true;
            subscription.handler(event.payload);
        }
        return matched;
    }

    /// Counts every drop but reports at most one summary per
    /// QUEUE_FULL_REPORT_PERIOD_MS.
    ///
    /// The counters are std::atomic touched only by load and store. That is lock-free
    /// on every 32-bit MCU, including cores without atomic read-modify-write
    /// (Cortex-M0, RV32 without the A extension), where an atomic increment or
    /// compare-and-swap compiles to a call into a libatomic that bare-metal
    /// toolchains do not ship. It also keeps a race between two pushing contexts well
    /// defined. What it gives up is exactness under that race: without a Lock, a drop
    /// landing in the same instant from two contexts can be counted once, and one
    /// window can see two reports. A Lock that is the platform's critical section
    /// makes both exact again.
    ///
    /// The callback runs outside the lock, so it is free to log.
    ///
    /// The period throttles the report, it is not a timing guarantee: `nowMs` is
    /// whatever resolution the caller's clock has, so a report can land a tick either
    /// side of the nominal period. Nothing here needs better than that.
    void ReportDrop(EventId id, std::uint32_t nowMs)
    {
        std::uint32_t dropped = 0U;
        {
            const detail::EventManagerGuard<Lock> guard{m_lock};
            const std::uint32_t count  = m_droppedSinceReport.load(std::memory_order_relaxed) + 1U;
            const std::uint32_t lastMs = m_lastReportMs.load(std::memory_order_relaxed);
            // Unsigned subtraction, so a 32-bit clock wrap costs at most one late report.
            if ((nowMs - lastMs) < QUEUE_FULL_REPORT_PERIOD_MS)
            {
                m_droppedSinceReport.store(count, std::memory_order_relaxed);
                return;
            }
            m_lastReportMs.store(nowMs, std::memory_order_relaxed);
            m_droppedSinceReport.store(0U, std::memory_order_relaxed);
            dropped = count;
        }
        if (m_onEventsDropped)
        {
            m_onEventsDropped(id, dropped);
        }
    }

    Queue& m_queue;
    std::array<Subscription, MAX_SUBSCRIPTIONS> m_subscriptions{};
    std::size_t m_count{0U};
    OnSubscriptionsFull m_onSubscriptionsFull;
    OnEventsDropped m_onEventsDropped;
    Lock m_lock{};
    std::atomic<std::uint32_t> m_droppedSinceReport{0U};
    std::atomic<std::uint32_t> m_lastReportMs{0U};
};

} // namespace hwlib::events
