// Keep test assertions active even when the runtime is built with NDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "Reclamation.h"
#include "globalConfig.h"
#include <atomic>
#include <cassert>
#include <csignal>
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>

// Standalone runtime test; also works with ASan's mmap-based allocator.
void *global_addr_start = nullptr;
const uintptr_t ADDRESS_MASK = (1ULL << 48) - 1;
SodaBitmap soda_((1ULL << 47) / 8);

struct Object
{
    std::atomic<int> &destroyed;
    int value = 42;
    ~Object() { ++destroyed; }
};

static void wait(std::future<void> &event)
{
    assert(event.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    event.get();
}

int main()
{
    rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore);
    std::atomic<int> destroyed{0};

    // A reader acquired the pointer before unlink. A later reader must not
    // hold up reclamation of an object it could never have discovered.
    auto *object = new Object{destroyed};
    std::atomic<Object *> published{object};
    std::promise<void> entered, leave, youngerEntered, youngerLeave;
    auto enteredFuture = entered.get_future();
    auto leaveFuture = leave.get_future();
    auto youngerEnteredFuture = youngerEntered.get_future();
    auto youngerLeaveFuture = youngerLeave.get_future();
    std::thread older([&] {
        reclamation::Scope scope;
        Object *borrowed = published.load();
        entered.set_value();
        wait(leaveFuture);
        assert(borrowed->value == 42);
    });
    wait(enteredFuture);
    CREST_RETIRE_DELETE(published.exchange(nullptr));
    assert(destroyed == 0 && reclamation::pending() == 1);
    std::thread younger([&] {
        reclamation::Scope scope;
        assert(published.load() == nullptr);
        youngerEntered.set_value();
        wait(youngerLeaveFuture);
    });
    wait(youngerEnteredFuture);
    leave.set_value();
    older.join();
    reclamation::collect();
    assert(destroyed == 1); // the younger reader is still active
    youngerLeave.set_value();
    younger.join();

    // Retirement can be requested before the last unlink in an update.
    // A reader entering in that interval must also finish before deletion.
    published.store(new Object{destroyed});
    std::promise<void> staged, finishUpdate, lateEntered, finishLate;
    auto stagedFuture = staged.get_future();
    auto finishUpdateFuture = finishUpdate.get_future();
    auto lateEnteredFuture = lateEntered.get_future();
    auto finishLateFuture = finishLate.get_future();
    std::thread updater([&] {
        reclamation::Scope scope;
        CREST_RETIRE_DELETE(published.load());
        staged.set_value();
        wait(finishUpdateFuture);
        published.store(nullptr);
    });
    wait(stagedFuture);
    std::thread late([&] {
        reclamation::Scope scope;
        Object *borrowed = published.load();
        lateEntered.set_value();
        wait(finishLateFuture);
        assert(borrowed->value == 42);
    });
    wait(lateEnteredFuture);
    finishUpdate.set_value();
    updater.join();
    assert(destroyed == 1);
    finishLate.set_value();
    late.join();
    assert(destroyed == 2);

    // Nested API calls and exception unwinding keep the outer protection.
    try
    {
        reclamation::Scope outer;
        auto *p = new Object{destroyed};
        {
            reclamation::Scope inner;
            CREST_RETIRE_DELETE(p);
        }
        assert(p->value == 42 && destroyed == 2);
        throw std::runtime_error("exercise unwind");
    }
    catch (const std::runtime_error &) {}
    assert(destroyed == 3);

    // Retiring a wrapper must unpublish its embedded Guide slots from SODA
    // immediately; an older OC reader still retains access until scope exit.
    auto *slot = new uintptr_t(123);
    const auto index = reinterpret_cast<uintptr_t>(slot) / 8;
    soda_.Set1(index);
    {
        reclamation::Scope scanner;
        CREST_RETIRE_DELETE(slot);
        assert(!soda_.Get(index));
        assert(*slot == 123 && reclamation::pending() == 1);
    }
    assert(reclamation::pending() == 0);

    // Destructors can retire children without deadlocking the queue.
    struct Parent
    {
        Object *child;
        ~Parent() { CREST_RETIRE_DELETE(child); }
    };
    CREST_RETIRE_DELETE(new Parent{new Object{destroyed}});
    reclamation::drain();
    assert(destroyed == 4 && reclamation::pending() == 0);
    CREST_RETIRE_FREE(nullptr);

    // Thread registration is reusable and has no fixed slot-count limit.
    for (int i = 0; i < 80; ++i)
    {
        std::thread worker([&] {
            reclamation::Scope scope;
            CREST_RETIRE_DELETE(new Object{destroyed});
        });
        worker.join();
    }
    assert(destroyed == 84);

    // Shutdown must also wait for a deleter already removed from the queue.
    std::promise<void> deleting, finishDelete;
    auto deletingFuture = deleting.get_future();
    auto finishDeleteFuture = finishDelete.get_future();
    struct SlowDelete
    {
        std::promise<void> &started;
        std::future<void> &finish;
        ~SlowDelete() { started.set_value(); wait(finish); }
    };
    std::thread collector([&] { CREST_RETIRE_DELETE((new SlowDelete{deleting, finishDeleteFuture})); });
    wait(deletingFuture);
    assert(reclamation::pending() == 1);
    finishDelete.set_value();
    collector.join();
    reclamation::drain();
    assert(reclamation::pending() == 0);

    // These safety failures must abort even when compiled with NDEBUG.
    auto expectAbort = [](int kind) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child)
        {
            reclamation::Scope scope;
            if (kind == 0)
            {
                auto *p = new int(1);
                CREST_RETIRE_DELETE(p);
                CREST_RETIRE_DELETE(p);
            }
            else if (kind == 1) reclamation::drain();
            else CREST_RETIRE_FREE(reinterpret_cast<void *>(1ULL << 60));
            std::_Exit(9);
        }
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    };
    expectAbort(0);
    expectAbort(1);
    expectAbort(2);
    reclamation::drain();
    std::cout << "reclamation lifetime and safety checks passed\n";
}
