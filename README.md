# event-manager

Publish/subscribe over a queue you supply: handlers per event id, dispatch on your own loop or thread.

Part of [hwlib](https://github.com/integra-lib) — architecture-independent C++20
components shared between firmware projects. Header-only,
no exceptions, no RTTI.

## Use it

```bash
git submodule add git@github.com:integra-lib/event-manager.git external/hwlib/event-manager
```

```cmake
add_subdirectory(external/hwlib/event-manager)
target_link_libraries(app PRIVATE Hwlib::event_manager)
```

```cpp
#include <hwlib/events/event_manager.hpp>
```

Each component carries its own include directory, so this header stays unreachable
until the component is linked: a forgotten dependency is a compile error rather than
a build that happens to work.

## The queue is yours

The manager does not own a queue, it borrows one. That is what keeps the component
architecture-independent: on a device the queue is an RTOS primitive, in a test it is
an array. An implementation satisfies `hwlib::events::EventQueueLike`:

```cpp
bool TryPush(const hwlib::events::Event<Payload>&);  // never blocks — reached from an ISR
bool TryPop(hwlib::events::Event<Payload>&);         // returns false when empty
bool PopBlocking(hwlib::events::Event<Payload>&);    // waits for an event
```

A Zephyr adapter over `k_msgq` is about twenty lines and belongs in the project, next
to the thread that drains it:

```cpp
template<typename Payload, std::size_t DEPTH>
class MsgqEventQueue
{
public:
    // k_msgq copies the event as raw bytes, so this queue — not the component —
    // is what requires a trivially copyable payload.
    static_assert(std::is_trivially_copyable_v<hwlib::events::Event<Payload>>);

    MsgqEventQueue()
    {
        k_msgq_init(&m_queue, reinterpret_cast<char*>(m_buffer.data()),
                    sizeof(hwlib::events::Event<Payload>), DEPTH);
    }

    bool TryPush(const hwlib::events::Event<Payload>& event)
    {
        return k_msgq_put(&m_queue, &event, K_NO_WAIT) == 0;
    }
    bool TryPop(hwlib::events::Event<Payload>& event)
    {
        return k_msgq_get(&m_queue, &event, K_NO_WAIT) == 0;
    }
    bool PopBlocking(hwlib::events::Event<Payload>& event)
    {
        return k_msgq_get(&m_queue, &event, K_FOREVER) == 0;
    }

private:
    alignas(alignof(hwlib::events::Event<Payload>))
        std::array<std::uint8_t, sizeof(hwlib::events::Event<Payload>) * DEPTH> m_buffer{};
    k_msgq m_queue{};
};
```

## Dispatch and drops

```cpp
MsgqEventQueue<AppPayload, 8> queue;
hwlib::events::EventManager<AppPayload, decltype(queue)> events{queue};

events.Subscribe(AppEvent::eTick, [](const AppPayload& payload) { OnTick(payload); });

// Push takes the time rather than reading a clock, so the throttling below is
// testable on the host. It never blocks and is safe to call from an interrupt.
std::ignore = events.Push(AppEvent::eTick, payload, k_uptime_get_32());

while (events.TryDispatch()) {}  // or block on events.Dispatch()
```

Two things are reported rather than logged, because a component that logs would have to
pick a logging backend and stop being architecture-independent:

```cpp
events.SetOnSubscriptionsFull([](std::uint8_t id) { LOG_WRN("subscriptions full (id=%u)", id); });
events.SetOnEventsDropped([](std::uint8_t id, std::uint32_t dropped) {
    LOG_WRN("queue full, %u dropped (last id=%u)", dropped, id);
});
```

A subscription past `MAX_SUBSCRIPTIONS` is dropped, not stored — the cap bounds static
storage, and exceeding it is a configuration error the integrator has to see. A full
queue drops the event, counts it, and reports the accumulated count at most once per
`QUEUE_FULL_REPORT_PERIOD_MS`: a drop is worth knowing about, each individual drop is
not, and an unthrottled report turns a loop that is already behind into one that is
further behind.

`SetOnEventsDropped` runs on whatever context called `Push()`, an interrupt included,
and always outside the lock below.

## Drops from several contexts at once

The drop counters are `std::atomic`, but only loaded and stored — never incremented or
compared-and-swapped in one step. Loads and stores are lock-free on every 32-bit MCU;
an atomic increment is not on cores without atomic read-modify-write, Cortex-M0 and
RV32 without the A extension, where it compiles to a call into a libatomic that
bare-metal toolchains do not ship. That keeps the component free of any particular
MCU, and a race between two pushing contexts well defined.

What the default gives up is exactness under such a race: a drop landing in the same
instant from a thread and an interrupt can be counted once, and one window can see two
reports. The count is a diagnostic, and that is usually fine. When it is not, pass the
platform's critical section as the fourth template argument, and both are exact again:

```cpp
struct IrqLock
{
    void lock() { m_key = irq_lock(); }
    void unlock() { irq_unlock(m_key); }
    unsigned m_key{};
};

hwlib::events::EventManager<AppPayload, decltype(queue), hwlib::events::DEFAULT_MAX_SUBSCRIPTIONS, IrqLock> events{queue};
```

The lock is taken only on a drop, around the few loads and stores of the accounting, and
released before the callback runs.

## Versioning

Every component is released on its own, tagged `vX.Y.Z`. Pre-1.0, a minor release may
break the API, which is why dependants accept a single minor.

```bash
git -C external/hwlib/event-manager fetch --tags
git -C external/hwlib/event-manager checkout v0.2.0
git add external/hwlib/event-manager && git commit -m "build: bump event-manager to v0.2.0"
```

## In a consumer's CI

The component is an ordinary submodule, so the build needs it checked out. On GitLab
that means `GIT_SUBMODULE_STRATEGY: normal` (or `recursive`) on every job that builds —
not only on the ones that run unit tests.

## Develop it

```bash
git submodule update --init          # ci-shared, needed by pre-commit
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```

Tests are built only when this repository is the top-level project, so a consumer
never builds them and never fetches GoogleTest.

The style configs are symlinks into the `ci-shared` submodule, and the pipeline comes
from the same place. On GitHub this repository carries a self-contained build-and-test
workflow instead: a workflow token cannot read another private repository, so neither
a shared workflow nor the submodule is reachable there. The shared setup is what
GitLab will use.
