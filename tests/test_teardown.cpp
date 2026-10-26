#include <stepwright/debug/live_tasks.hpp>

#include <gtest/gtest.h>

#include "test_support.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

namespace {
// The notification lives outside the allocation being destroyed. In particular,
// joining the producer must happen AFTER destruction, not before it: a join would
// hide exactly the missing happens-before edge these tests exercise.
template <class T, class F>
void observe_and_destroy(std::unique_ptr<T>& owner, test_support::receiver& done, F action) {
    std::thread producer([object = owner.get(), action] { action(*object); });
    done.wait();
    owner.reset();
    producer.join();
    EXPECT_EQ(done.calls, 1);
}

struct signal_item : stepwright::work_item {
    explicit signal_item(std::atomic<bool>& signal) : signal_(&signal) {
        run = [](stepwright::work_item* item) noexcept {
            auto* target = static_cast<signal_item*>(item)->signal_;
            target->store(true, std::memory_order_release);
        };
    }
    std::atomic<bool>* signal_;
};

struct busy_item : stepwright::work_item {
    std::atomic<bool> entered{false};
    std::atomic<bool> stop{false};
    busy_item() {
        run = [](stepwright::work_item* item) noexcept {
            auto& self = *static_cast<busy_item*>(item);
            self.entered.store(true, std::memory_order_release);
            while (!self.stop.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        };
    }
};

struct signal_timer : stepwright::timer_service::entry {
    explicit signal_timer(std::atomic<bool>& signal) : signal_(&signal) {
        fire = [](stepwright::timer_service::entry* node) noexcept {
            auto* target = static_cast<signal_timer*>(node)->signal_;
            target->store(true, std::memory_order_release);
        };
    }
    std::atomic<bool>* signal_;
};

void await(const std::atomic<bool>& signal) {
    while (!signal.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

int rounds() {
    return test_support::stress_iterations(24);
}

struct deferred : stepwright::machine<deferred> {
    step begin() { return defer<&deferred::finish>(); }
    step finish() { return done(); }
};

struct waking_source : stepwright::source_machine<waking_source, int> {
    bool terminal = false;
    step begin() { return park<&waking_source::finish>(); }
    step finish() { return terminal ? end() : emit<&waking_source::last>(42); }
    step last() { return end(); }
};

template <bool Any>
struct joined {
    test_support::cancellable_child first;
    test_support::cancellable_child second;
    std::conditional_t<Any, stepwright::when_any<2>, stepwright::when_all<2>> operation{first,
                                                                                        second};
};

struct deadline_task {
    test_support::cancellable_child inner;
    stepwright::with_deadline operation;
    deadline_task(stepwright::timer_service& timers, std::chrono::steady_clock::duration timeout)
        : operation(inner, timers, timeout) {}
};

// Force the publication race without relying on the OS timer's scheduling: the
// timer thread finishes before schedule returns. Only service storage is touched
// after firing; the callback is allowed to destroy its entry immediately.
struct fire_before_return_timer : stepwright::timer_service {
    stepwright::testing::manual_timer_service timers;
    bool schedule_returned = false;
    clock::time_point now() const noexcept override { return timers.now(); }
    void schedule(entry& e, clock::time_point deadline) noexcept override {
        timers.schedule(e, deadline);
        std::thread driver([this, deadline] { timers.advance_to(deadline); });
        driver.join();
        schedule_returned = true;
    }
    bool cancel(entry& e) noexcept override { return timers.cancel(e); }
};

template <class T>
struct reclaiming_wait {
    std::unique_ptr<T> object;
    fire_before_return_timer& timers;
    test_support::receiver done;
    bool completed_before_return = false;
    void finish(const stepwright::error& result) noexcept {
        completed_before_return = !timers.schedule_returned;
        object.reset();
        done.finish(result);
    }
};
} // namespace

TEST(teardown, delay_completion_before_schedule_returns) {
    for (bool cancellable : {false, true}) {
        fire_before_return_timer timers;
        stepwright::cancel_source source;
        reclaiming_wait<stepwright::delay> wait{
            .object = std::make_unique<stepwright::delay>(timers, 0ms),
            .timers = timers,
            .done = {}};
        wait.object->start(stepwright::completion::bind<&decltype(wait)::finish>(&wait),
                           cancellable ? source.token() : stepwright::cancel_token{});
        EXPECT_FALSE(wait.object);
        EXPECT_EQ(wait.done.calls, 1);
        EXPECT_FALSE(wait.done.result);
        EXPECT_EQ(wait.completed_before_return, !cancellable);
    }
}

TEST(teardown, interval_completion_before_schedule_returns) {
    for (bool cancellable : {false, true}) {
        fire_before_return_timer timers;
        stepwright::cancel_source source;
        reclaiming_wait<stepwright::interval_source> wait{
            .object = std::make_unique<stepwright::interval_source>(timers, 1ms),
            .timers = timers,
            .done = {}};
        wait.object->on_ready(stepwright::completion::bind<&decltype(wait)::finish>(&wait),
                              cancellable ? source.token() : stepwright::cancel_token{});
        EXPECT_FALSE(wait.object);
        EXPECT_EQ(wait.done.calls, 1);
        EXPECT_FALSE(wait.done.result);
        EXPECT_EQ(wait.completed_before_return, !cancellable);
    }
}

TEST(teardown, outside_post_before_poster_returns) {
    for (int i = 0; i < test_support::stress_iterations(256); ++i) {
        auto pool = std::make_unique<stepwright::thread_pool>(4);
        busy_item busy;
        pool->post(busy);
        await(busy.entered);
        // A running worker keeps the other workers' timed rescans alive, so they
        // can discover injection without waiting for the poster's notification.
        std::this_thread::sleep_for(300us);
        std::atomic<bool> ran{false};
        signal_item item(ran);
        std::thread poster([raw = pool.get(), &item] { raw->post(item); });
        await(ran);
        busy.stop.store(true, std::memory_order_release);
        if (i % 2 == 0) {
            pool->wait_idle();
        }
        pool.reset();
        poster.join();
    }
}

TEST(teardown, due_timer_before_scheduler_returns) {
    for (int i = 0; i < test_support::stress_iterations(512); ++i) {
        auto timers = std::make_unique<stepwright::steady_timer_service>();
        std::atomic<bool> fired{false};
        signal_timer entry(fired);
        std::thread scheduler([raw = timers.get(), &entry] {
            raw->schedule(entry, stepwright::timer_service::clock::now());
        });
        await(fired);
        timers.reset();
        scheduler.join();
    }
}

TEST(teardown, event_set_and_cancel) {
    for (int i = 0; i < rounds(); ++i) {
        auto ev = std::make_unique<stepwright::event>();
        auto waiter = std::make_unique<stepwright::event::waiter>(*ev);
        test_support::receiver done;
        waiter->start(done.callback());
        std::thread setter([raw = ev.get()] { raw->set(); });
        done.wait();
        waiter.reset();
        ev.reset();
        setter.join();
        EXPECT_EQ(done.calls, 1);

        ev = std::make_unique<stepwright::event>();
        waiter = std::make_unique<stepwright::event::waiter>(*ev);
        auto source = std::make_unique<stepwright::cancel_source>();
        test_support::receiver cancelled;
        waiter->start(cancelled.callback(), source->token());
        observe_and_destroy(source, cancelled, [](auto& object) { object.request_cancel(); });
        waiter.reset();
        ev.reset();
        EXPECT_TRUE(cancelled.result.is(stepwright::errc::cancelled));
    }
}

TEST(teardown, mutex_handoff) {
    for (int i = 0; i < rounds(); ++i) {
        auto mutex = std::make_unique<stepwright::async_mutex>();
        ASSERT_TRUE(mutex->try_lock());
        auto waiter = std::make_unique<stepwright::async_mutex::lock_op>(*mutex);
        test_support::receiver done;
        waiter->start(done.callback());
        std::thread releaser([raw = mutex.get()] { raw->unlock(); });
        done.wait();
        mutex->unlock();
        waiter.reset();
        mutex.reset();
        releaser.join();
    }
}

TEST(teardown, shared_mutex_reader_batch) {
    for (int i = 0; i < rounds(); ++i) {
        auto mutex = std::make_unique<stepwright::async_shared_mutex>();
        ASSERT_TRUE(mutex->try_lock());
        auto a = std::make_unique<stepwright::async_shared_mutex::shared_lock_op>(*mutex);
        auto b = std::make_unique<stepwright::async_shared_mutex::shared_lock_op>(*mutex);
        test_support::receiver first;
        test_support::receiver last;
        a->start(first.callback());
        b->start(last.callback());
        std::thread releaser([raw = mutex.get()] { raw->unlock(); });
        first.wait();
        last.wait();
        mutex->unlock_shared();
        mutex->unlock_shared();
        a.reset();
        b.reset();
        mutex.reset();
        releaser.join();
    }
}

TEST(teardown, shared_task_completion_and_cancellation) {
    for (int i = 0; i < rounds(); ++i) {
        test_support::cancellable_child inner;
        auto shared = std::make_unique<stepwright::shared_task>(inner);
        auto waiter = std::make_unique<stepwright::shared_task::waiter>(*shared);
        test_support::receiver done;
        waiter->start(done.callback());
        std::thread producer([raw = shared.get(), &inner, i] {
            if (i % 2 == 0) {
                raw->cancel();
            } else {
                inner.complete();
            }
        });
        done.wait();
        waiter.reset();
        shared.reset();
        producer.join();
        EXPECT_EQ(done.calls, 1);
    }
}

TEST(teardown, channel_send_close_and_readiness_cancel) {
    for (int i = 0; i < rounds(); ++i) {
        auto channel = std::make_unique<stepwright::channel<int>>(1);
        test_support::receiver done;
        stepwright::cancel_source source;
        channel->on_ready(done.callback(), source.token());
        if (i % 3 == 0) {
            observe_and_destroy(channel, done, [](auto& object) { object.try_send(42); });
        } else if (i % 3 == 1) {
            observe_and_destroy(channel, done, [](auto& object) { object.close(); });
        } else {
            std::thread canceller([&source] { source.request_cancel(); });
            done.wait();
            channel.reset();
            canceller.join();
        }
    }
}

TEST(teardown, channel_receive_and_close_release_sender) {
    for (int i = 0; i < rounds(); ++i) {
        auto channel = std::make_unique<stepwright::channel<int>>(1);
        ASSERT_TRUE(channel->try_send(1));
        auto sender = std::make_unique<stepwright::channel<int>::send_op>(*channel, 2);
        test_support::receiver done;
        sender->start(done.callback());
        std::thread producer([raw = channel.get(), i] {
            if (i % 2 == 0) {
                raw->try_pull();
            } else {
                raw->close();
            }
        });
        done.wait();
        sender.reset();
        channel.reset();
        producer.join();
        EXPECT_EQ(done.calls, 1);
    }
}

TEST(teardown, broadcast_publish_close_and_unsubscribe) {
    using broadcast = stepwright::broadcast<int>;
    for (int i = 0; i < rounds(); ++i) {
        auto owner = std::make_unique<broadcast>(1);
        auto sub = std::make_unique<broadcast::subscription>(*owner);
        test_support::receiver ready;
        sub->on_ready(ready.callback(), {});
        std::thread publisher([raw = owner.get(), i] {
            if (i % 2 == 0) {
                raw->try_publish(1);
            } else {
                raw->close();
            }
        });
        ready.wait();
        sub.reset();
        owner.reset();
        publisher.join();

        owner = std::make_unique<broadcast>(1);
        sub = std::make_unique<broadcast::subscription>(*owner);
        ASSERT_TRUE(owner->try_publish(1));
        auto pending = std::make_unique<broadcast::publish_op>(*owner, 2);
        test_support::receiver done;
        pending->start(done.callback());
        std::thread unsubscriber([subscription = sub.release()] { delete subscription; });
        done.wait();
        pending.reset();
        owner.reset();
        unsubscriber.join();
    }
}

TEST(teardown, machine_wake_and_home_hop) {
    for (int i = 0; i < rounds(); ++i) {
        stepwright::thread_pool pool(2);
        auto machine = std::make_unique<test_support::parked>();
        if (i % 2 == 0) {
            machine->bind(pool);
        }
        test_support::receiver done;
        machine->start(done.callback());
        observe_and_destroy(machine, done, [i](auto& object) {
            if (i % 3 == 0) {
                object.wake_if_parked();
            } else {
                object.wake();
            }
        });
    }
}

TEST(teardown, delay_start_and_cancel) {
    for (int i = 0; i < rounds(); ++i) {
        auto timers = std::make_unique<stepwright::steady_timer_service>();
        auto delay = std::make_unique<stepwright::delay>(*timers, i % 2 == 0 ? 0ms : 1h);
        test_support::receiver done;
        stepwright::cancel_source source;
        if (i % 2 == 0) {
            observe_and_destroy(delay, done,
                                [&done](auto& object) { object.start(done.callback()); });
        } else {
            delay->start(done.callback(), source.token());
            std::thread canceller([&source] { source.request_cancel(); });
            done.wait();
            delay.reset();
            timers.reset();
            canceller.join();
        }
    }
}

TEST(teardown, cancellation_registration_activation_and_dispatch) {
    for (int i = 0; i < rounds(); ++i) {
        auto source = std::make_unique<stepwright::cancel_source>();
        auto registration = std::make_unique<stepwright::cancel_registration>();
        test_support::receiver done;
        const auto notify = [](void* context) noexcept {
            static_cast<test_support::receiver*>(context)->finish({});
        };
        if (i % 2 == 0) {
            registration->arm(source->token(), notify, &done);
            observe_and_destroy(source, done, [](auto& object) { object.request_cancel(); });
            registration.reset();
        } else {
            source->request_cancel();
            auto token = source->token();
            observe_and_destroy(registration, done, [&token, &done, notify](auto& object) {
                object.arm(token, notify, &done);
            });
        }
    }
}

TEST(teardown, joins_wait_for_dispatch_before_completing) {
    for (int i = 0; i < rounds(); ++i) {
        auto all = std::make_unique<joined<false>>();
        test_support::receiver all_done;
        all->operation.start(all_done.callback());
        all->first.complete();
        observe_and_destroy(all, all_done, [](auto& object) { object.second.complete(); });

        auto any = std::make_unique<joined<true>>();
        test_support::receiver any_done;
        any->operation.start(any_done.callback());
        observe_and_destroy(any, any_done, [](auto& object) { object.first.complete(); });
    }
}

TEST(teardown, deadline_inner_timeout_and_parent_cancel) {
    for (int i = 0; i < rounds(); ++i) {
        auto timers = std::make_unique<stepwright::steady_timer_service>();
        auto owner = std::make_unique<deadline_task>(*timers, i % 3 == 0 ? 1ms : 1h);
        test_support::receiver done;
        stepwright::cancel_source source;
        owner->operation.start(done.callback(), source.token());
        if (i % 3 == 0) {
            done.wait();
            owner.reset();
        } else if (i % 3 == 1) {
            observe_and_destroy(owner, done, [](auto& object) { object.inner.complete(); });
        } else {
            std::thread canceller([&source] { source.request_cancel(); });
            done.wait();
            owner.reset();
            timers.reset();
            canceller.join();
        }
    }
}

TEST(teardown, merge_readiness_and_cancellation) {
    for (int i = 0; i < rounds(); ++i) {
        stepwright::channel<int> a(1);
        stepwright::channel<int> b(1);
        auto merged = std::make_unique<stepwright::merge<int, 2>>(a, b);
        test_support::receiver done;
        stepwright::cancel_source source;
        merged->on_ready(done.callback(), source.token());
        std::thread producer([&a, &source, i] {
            if (i % 2 == 0) {
                a.try_send(1);
            } else {
                source.request_cancel();
            }
        });
        done.wait();
        merged.reset();
        producer.join();
    }
}

TEST(teardown, source_machine_emit_and_end) {
    for (int i = 0; i < rounds(); ++i) {
        auto source = std::make_unique<waking_source>();
        source->terminal = i % 2 == 0;
        EXPECT_EQ(source->try_pull().status(), stepwright::pull<int>::state::pending);
        test_support::receiver ready;
        source->on_ready(ready.callback(), {});
        observe_and_destroy(source, ready, [](auto& object) { object.wake(); });
    }
}

TEST(teardown, interval_readiness_and_cancel) {
    for (int i = 0; i < rounds(); ++i) {
        auto timers = std::make_unique<stepwright::steady_timer_service>();
        auto source = std::make_unique<stepwright::interval_source>(*timers, i % 2 == 0 ? 1ms : 1h);
        test_support::receiver ready;
        if (i % 2 == 0) {
            observe_and_destroy(source, ready,
                                [&ready](auto& object) { object.on_ready(ready.callback(), {}); });
        } else {
            stepwright::cancel_source cancellation;
            source->on_ready(ready.callback(), cancellation.token());
            std::thread canceller([&cancellation] { cancellation.request_cancel(); });
            ready.wait();
            source.reset();
            timers.reset();
            canceller.join();
        }
    }
}

TEST(teardown, defer_and_registry_snapshot) {
    for (int i = 0; i < rounds(); ++i) {
        stepwright::thread_pool pool(2);
        auto task = std::make_unique<deferred>();
        task->bind(pool);
        test_support::receiver done;
        std::atomic<bool> stop{false};
        std::thread snapshots([&stop] {
            while (!stop.load(std::memory_order_acquire)) {
                stepwright::debug::for_each_live([](const auto&) {});
            }
        });
        observe_and_destroy(task, done, [&done](auto& object) { object.start(done.callback()); });
        stop.store(true, std::memory_order_release);
        snapshots.join();
    }
}

TEST(teardown, manual_and_virtual_drivers_preserve_callback_storage) {
    for (int i = 0; i < rounds(); ++i) {
        // Driving calls are caller-owned loops, not background threads joined by
        // destruction. Keep the service alive until the driving call returns;
        // the entry/task itself may be destroyed as soon as completion is seen.
        stepwright::testing::manual_timer_service manual;
        auto delay = std::make_unique<stepwright::delay>(manual, 0ms);
        test_support::receiver done;
        delay->start(done.callback());
        std::thread driver([&manual] { manual.advance(0ms); });
        done.wait();
        delay.reset();
        driver.join();

        auto executor = std::make_unique<stepwright::testing::virtual_executor>();
        std::atomic<bool> ran{false};
        signal_timer timer(ran);
        std::thread scheduler([raw = executor.get(), &timer] {
            raw->schedule(timer, stepwright::timer_service::clock::time_point{});
        });
        while (!executor->run_one()) {
            std::this_thread::yield();
        }
        EXPECT_TRUE(ran.load(std::memory_order_acquire));
        executor.reset();
        scheduler.join();

        auto fifo = std::make_unique<stepwright::manual_executor>();
        ran.store(false, std::memory_order_relaxed);
        signal_item item(ran);
        std::thread poster([raw = fifo.get(), &item] { raw->post(item); });
        while (!fifo->run_one()) {
            std::this_thread::yield();
        }
        fifo.reset();
        poster.join();
    }
}

TEST(teardown, broadcast_pull_and_readiness_cancel) {
    using broadcast = stepwright::broadcast<int>;
    for (int i = 0; i < rounds(); ++i) {
        auto owner = std::make_unique<broadcast>(1);
        auto sub = std::make_unique<broadcast::subscription>(*owner);
        stepwright::cancel_source source;
        test_support::receiver ready;
        sub->on_ready(ready.callback(), source.token());
        std::thread canceller([&source] { source.request_cancel(); });
        ready.wait();
        sub.reset();
        canceller.join();

        sub = std::make_unique<broadcast::subscription>(*owner);
        ASSERT_TRUE(owner->try_publish(1));
        auto pending = std::make_unique<broadcast::publish_op>(*owner, 2);
        test_support::receiver done;
        pending->start(done.callback());
        std::thread consumer([raw = sub.get()] { raw->try_pull(); });
        done.wait();
        pending.reset();
        sub.reset();
        owner.reset();
        consumer.join();
    }
}

TEST(teardown, queued_sender_and_shared_mutex_cancellation) {
    for (int i = 0; i < rounds(); ++i) {
        auto channel = std::make_unique<stepwright::channel<int>>(1);
        ASSERT_TRUE(channel->try_send(1));
        auto sender = std::make_unique<stepwright::channel<int>::send_op>(*channel, 2);
        stepwright::cancel_source source;
        test_support::receiver done;
        sender->start(done.callback(), source.token());
        std::thread canceller([&source] { source.request_cancel(); });
        done.wait();
        sender.reset();
        channel.reset();
        canceller.join();

        auto mutex = std::make_unique<stepwright::async_shared_mutex>();
        ASSERT_TRUE(mutex->try_lock_shared());
        auto writer = std::make_unique<stepwright::async_shared_mutex::lock_op>(*mutex);
        auto reader = std::make_unique<stepwright::async_shared_mutex::shared_lock_op>(*mutex);
        stepwright::cancel_source cancellation;
        test_support::receiver cancelled;
        test_support::receiver granted;
        writer->start(cancelled.callback(), cancellation.token());
        reader->start(granted.callback());
        std::thread requester([&cancellation] { cancellation.request_cancel(); });
        cancelled.wait();
        granted.wait();
        mutex->unlock_shared();
        mutex->unlock_shared();
        reader.reset();
        writer.reset();
        mutex.reset();
        requester.join();
    }
}
