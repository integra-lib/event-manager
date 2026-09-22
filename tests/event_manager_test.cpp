#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <integra/event_manager.hpp>
#include <string>
#include <vector>

namespace
{

using integra::Event;
using integra::EventManager;
using integra::QUEUE_FULL_REPORT_PERIOD_MS;

// Stand-in for the platform queue. Single-threaded, so PopBlocking cannot block:
// a test that would have to wait has nothing to wait for, and a would-be block is
// reported as an empty pop instead of hanging the suite.
template<typename Payload, std::size_t CAPACITY>
class FakeQueue
{
public:
    [[nodiscard]] bool TryPush(const Event<Payload>& event)
    {
        if (m_events.size() >= CAPACITY)
        {
            return false;
        }
        m_events.push_back(event);
        return true;
    }

    [[nodiscard]] bool TryPop(Event<Payload>& event)
    {
        if (m_events.empty())
        {
            return false;
        }
        event = m_events.front();
        m_events.erase(m_events.begin());
        return true;
    }

    [[nodiscard]] bool PopBlocking(Event<Payload>& event)
    {
        return TryPop(event);
    }

private:
    std::vector<Event<Payload>> m_events;
};

enum class TestEvent : std::uint8_t
{
    eTick  = 1U,
    eAlarm = 2U,
};

TEST(EventManagerTest, DeliversPayloadToItsSubscriber)
{
    FakeQueue<int, 4> queue;
    EventManager<int, FakeQueue<int, 4>> manager{queue};

    int received = 0;
    manager.Subscribe(TestEvent::eTick, [&received](const int& value) { received = value; });

    EXPECT_TRUE(manager.Push(TestEvent::eTick, 42, 0U));
    EXPECT_TRUE(manager.TryDispatch());
    EXPECT_EQ(received, 42);
}

TEST(EventManagerTest, DeliversToEverySubscriberOfTheSameId)
{
    FakeQueue<int, 4> queue;
    EventManager<int, FakeQueue<int, 4>> manager{queue};

    int calls = 0;
    manager.Subscribe(TestEvent::eTick, [&calls](const int&) { ++calls; });
    manager.Subscribe(TestEvent::eTick, [&calls](const int&) { ++calls; });

    EXPECT_TRUE(manager.Push(TestEvent::eTick, 1, 0U));
    EXPECT_TRUE(manager.TryDispatch());
    EXPECT_EQ(calls, 2);
}

TEST(EventManagerTest, LeavesOtherIdsAlone)
{
    FakeQueue<int, 4> queue;
    EventManager<int, FakeQueue<int, 4>> manager{queue};

    bool called = false;
    manager.Subscribe(TestEvent::eAlarm, [&called](const int&) { called = true; });

    EXPECT_TRUE(manager.Push(TestEvent::eTick, 1, 0U));
    // The event was pulled off the queue, so the drain loop keeps going...
    EXPECT_TRUE(manager.TryDispatch());
    // ...but nothing was subscribed to it.
    EXPECT_FALSE(called);
}

TEST(EventManagerTest, TryDispatchReportsAnEmptyQueue)
{
    FakeQueue<int, 4> queue;
    EventManager<int, FakeQueue<int, 4>> manager{queue};

    EXPECT_FALSE(manager.TryDispatch());
}

TEST(EventManagerTest, DispatchReportsWhetherAHandlerMatched)
{
    FakeQueue<int, 4> queue;
    EventManager<int, FakeQueue<int, 4>> manager{queue};

    manager.Subscribe(TestEvent::eTick, [](const int&) {});

    EXPECT_TRUE(manager.Push(TestEvent::eTick, 1, 0U));
    EXPECT_TRUE(manager.Dispatch());

    EXPECT_TRUE(manager.Push(TestEvent::eAlarm, 1, 0U));
    EXPECT_FALSE(manager.Dispatch());
}

TEST(EventManagerTest, DropsASubscriptionPastTheCapAndSaysSo)
{
    FakeQueue<int, 4> queue;
    EventManager<int, FakeQueue<int, 4>, 1U> manager{queue};

    std::vector<std::uint8_t> refused;
    manager.SetOnSubscriptionsFull([&refused](std::uint8_t id) { refused.push_back(id); });

    int firstCalls  = 0;
    int secondCalls = 0;
    manager.Subscribe(TestEvent::eTick, [&firstCalls](const int&) { ++firstCalls; });
    manager.Subscribe(TestEvent::eTick, [&secondCalls](const int&) { ++secondCalls; });

    ASSERT_EQ(refused.size(), 1U);
    EXPECT_EQ(refused.front(), static_cast<std::uint8_t>(TestEvent::eTick));

    EXPECT_TRUE(manager.Push(TestEvent::eTick, 1, 0U));
    EXPECT_TRUE(manager.TryDispatch());
    EXPECT_EQ(firstCalls, 1);
    EXPECT_EQ(secondCalls, 0);
}

TEST(EventManagerTest, PushFailsOnAFullQueue)
{
    FakeQueue<int, 1> queue;
    EventManager<int, FakeQueue<int, 1>> manager{queue};

    EXPECT_TRUE(manager.Push(TestEvent::eTick, 1, 0U));
    EXPECT_FALSE(manager.Push(TestEvent::eTick, 2, 0U));
}

TEST(EventManagerTest, HoldsBackTheDropReportUntilThePeriodHasPassed)
{
    FakeQueue<int, 1> queue;
    EventManager<int, FakeQueue<int, 1>> manager{queue};

    std::vector<std::uint32_t> reports;
    manager.SetOnEventsDropped([&reports](std::uint8_t, std::uint32_t dropped) { reports.push_back(dropped); });

    EXPECT_TRUE(manager.Push(TestEvent::eTick, 1, 0U));
    EXPECT_FALSE(manager.Push(TestEvent::eTick, 2, 0U));
    EXPECT_FALSE(manager.Push(TestEvent::eTick, 3, QUEUE_FULL_REPORT_PERIOD_MS - 1U));
    EXPECT_TRUE(reports.empty());

    EXPECT_FALSE(manager.Push(TestEvent::eTick, 4, QUEUE_FULL_REPORT_PERIOD_MS));
    ASSERT_EQ(reports.size(), 1U);
    // All three drops are in the report, not just the one that tripped the window.
    EXPECT_EQ(reports.front(), 3U);
}

TEST(EventManagerTest, CountsFromZeroAfterAReport)
{
    FakeQueue<int, 1> queue;
    EventManager<int, FakeQueue<int, 1>> manager{queue};

    std::vector<std::uint32_t> reports;
    manager.SetOnEventsDropped([&reports](std::uint8_t, std::uint32_t dropped) { reports.push_back(dropped); });

    EXPECT_TRUE(manager.Push(TestEvent::eTick, 1, 0U));
    EXPECT_FALSE(manager.Push(TestEvent::eTick, 2, 0U));
    EXPECT_FALSE(manager.Push(TestEvent::eTick, 3, QUEUE_FULL_REPORT_PERIOD_MS));
    ASSERT_EQ(reports.size(), 1U);
    EXPECT_EQ(reports.front(), 2U);

    EXPECT_FALSE(manager.Push(TestEvent::eTick, 4, 2U * QUEUE_FULL_REPORT_PERIOD_MS));
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(reports.back(), 1U);
}

TEST(EventManagerTest, SurvivesAClockWrap)
{
    FakeQueue<int, 1> queue;
    EventManager<int, FakeQueue<int, 1>> manager{queue};

    std::vector<std::uint32_t> reports;
    manager.SetOnEventsDropped([&reports](std::uint8_t, std::uint32_t dropped) { reports.push_back(dropped); });

    constexpr std::uint32_t BEFORE_WRAP = 0xFFFFFFFFU - 100U;
    EXPECT_TRUE(manager.Push(TestEvent::eTick, 1, BEFORE_WRAP));
    EXPECT_FALSE(manager.Push(TestEvent::eTick, 2, BEFORE_WRAP));
    ASSERT_EQ(reports.size(), 1U);

    // 50 ms later, still short of the wrap. The window has to be measured as an
    // unsigned difference from the last report: computing `lastMs + PERIOD` instead
    // overflows here and lets the report out early.
    EXPECT_FALSE(manager.Push(TestEvent::eTick, 3, BEFORE_WRAP + 50U));
    EXPECT_EQ(reports.size(), 1U);

    // 200 ms after the last report the 32-bit clock has wrapped past zero: time must
    // not appear to run backwards.
    const std::uint32_t afterWrap = BEFORE_WRAP + 200U;
    EXPECT_FALSE(manager.Push(TestEvent::eTick, 3, afterWrap));
    EXPECT_EQ(reports.size(), 1U);
    EXPECT_FALSE(manager.Push(TestEvent::eTick, 4, afterWrap + QUEUE_FULL_REPORT_PERIOD_MS));
    EXPECT_EQ(reports.size(), 2U);
}

TEST(EventManagerTest, ReportsWithoutACallbackDoNotCrash)
{
    FakeQueue<int, 1> queue;
    EventManager<int, FakeQueue<int, 1>, 1U> manager{queue};

    manager.Subscribe(TestEvent::eTick, [](const int&) {});
    manager.Subscribe(TestEvent::eAlarm, [](const int&) {});

    EXPECT_TRUE(manager.Push(TestEvent::eTick, 1, 0U));
    EXPECT_FALSE(manager.Push(TestEvent::eTick, 2, QUEUE_FULL_REPORT_PERIOD_MS));
}

TEST(EventManagerTest, CarriesAPayloadThatIsNotTriviallyCopyable)
{
    // The queue decides whether a payload must be trivially copyable; this one
    // does not, so a std::string payload has to work.
    FakeQueue<std::string, 4> queue;
    EventManager<std::string, FakeQueue<std::string, 4>> manager{queue};

    std::string received;
    manager.Subscribe(TestEvent::eTick, [&received](const std::string& value) { received = value; });

    EXPECT_TRUE(manager.Push(TestEvent::eTick, std::string{"payload"}, 0U));
    EXPECT_TRUE(manager.TryDispatch());
    EXPECT_EQ(received, "payload");
}

} // namespace
