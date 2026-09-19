// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <Base/Observer.h>

namespace
{

class CountingObserver: public Base::Observer<int>
{
public:
    explicit CountingObserver(std::atomic<int>& count)
        : count_(count)
    {}

    void OnChange(Base::Subject<int>& /*caller*/, int /*reason*/) override
    {
        count_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::atomic<int>& count_;
};

// An observer that detaches another observer while it is being notified must not
// invalidate the iteration.
class DetachingObserver: public Base::Observer<int>
{
public:
    DetachingObserver(Base::Subject<int>& subject, Base::Observer<int>* victim)
        : subject_(subject)
        , victim_(victim)
    {}

    void OnChange(Base::Subject<int>& /*caller*/, int /*reason*/) override
    {
        if (victim_) {
            subject_.Detach(victim_);
            victim_ = nullptr;
        }
    }

private:
    Base::Subject<int>& subject_;
    Base::Observer<int>* victim_;
};

}  // namespace

TEST(Subject, NotifyWithObserverDetachingDuringNotification)
{
    Base::Subject<int> subject;
    std::atomic<int> count {0};

    CountingObserver victim(count);
    DetachingObserver detacher(subject, &victim);

    subject.Attach(&victim);
    subject.Attach(&detacher);

    subject.Notify(0);
    EXPECT_EQ(count.load(), 1);

    subject.Detach(&victim);
    subject.Detach(&detacher);
}

TEST(Subject, ConcurrentAttachDetachAndNotify)
{
    Base::Subject<int> subject;
    std::atomic<int> count {0};
    std::atomic<bool> stop {false};

    CountingObserver observer(count);

    std::thread notifier([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            subject.Notify(0);
        }
    });

    std::thread mutator([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            subject.Attach(&observer);
            subject.Detach(&observer);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true, std::memory_order_relaxed);

    notifier.join();
    mutator.join();

    subject.Detach(&observer);
}
