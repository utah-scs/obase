#include "Reclamation.h"
#include "globalConfig.h"
#include <jemalloc/jemalloc.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace reclamation
{
    namespace
    {
        struct Retired
        {
            void *pointer;
            size_t bytes;
            Deleter deleter;
            uintptr_t context;
            uint64_t epoch;
            const char *file;
            int line;
        };
        struct Participant
        {
            uint64_t epoch = 0; // protected by Domain::mutex; zero is inactive
            unsigned depth = 0; // only accessed by this thread
            bool registered = false;
            bool collecting = false;
            std::vector<Retired> staged;
            ~Participant();
        };
        struct Domain
        {
            std::mutex mutex;
            uint64_t epoch = 1;
            std::vector<Participant *> participants;
            std::deque<Retired> retired;
            std::unordered_set<void *> pending_addresses;
            size_t outstanding = 0; // staged + queued + deleters in progress
        };

        Domain &domain()
        {
            // Intentionally process-lived: TLS destructors can run after
            // unrelated globals have been destroyed.
            static Domain *state = new Domain;
            return *state;
        }

        Participant &participant()
        {
            static thread_local Participant state;
            return state;
        }

        [[noreturn]] void violation(const char *reason, void *pointer,
                                   const char *file, int line)
        {
            std::fprintf(stderr, "RECLAMATION SAFETY FAILURE: %s pointer=%p at %s:%d\n",
                         reason, pointer, file, line);
            std::abort();
        }

        void validateAddress(void *pointer, const char *file, int line)
        {
            if (reinterpret_cast<uintptr_t>(pointer) & ~ADDRESS_MASK)
                violation("tagged or non-user free address", pointer, file, line);
        }

        // SODA is another publication path for Guide-containing objects.
        // Unregister their slots BEFORE starting the grace period, so a new
        // OC scan cannot acquire a pointer into an already-retired wrapper.
        void unpublishGuides(void *pointer, size_t bytes)
        {
            const auto address = reinterpret_cast<uintptr_t>(pointer);
            const auto base = reinterpret_cast<uintptr_t>(global_addr_start);
            if (!bytes || address < base) return;
            uint64_t index = (address - base + 7) / 8;
            uint64_t limit = (address - base + bytes) / 8;
            limit = std::min(limit, soda_.numbits());
            while (index < limit && soda_.FindNextSetBitBeforeLimit(&index, limit) && index < limit)
            {
                soda_.Set0(index++);
            }
        }

        uint64_t oldestReader(const Domain &state)
        {
            uint64_t oldest = std::numeric_limits<uint64_t>::max();
            for (const auto *reader : state.participants)
                if (reader->epoch) oldest = std::min(oldest, reader->epoch);
            return oldest;
        }

        Participant::~Participant()
        {
            if (depth) violation("thread exited inside an operation", nullptr, __FILE__, __LINE__);
            if (!registered) return;
            auto &state = domain();
            std::lock_guard<std::mutex> lock(state.mutex);
            auto &readers = state.participants;
            readers.erase(std::remove(readers.begin(), readers.end(), this), readers.end());
        }
    }

    Scope::Scope()
    {
        auto &reader = participant();
        if (reader.depth != 0) { ++reader.depth; return; }
        auto &state = domain();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!reader.registered)
        {
            state.participants.push_back(&reader);
            reader.registered = true;
        }
        reader.epoch = state.epoch;
        reader.depth = 1;
    }

    Scope::~Scope()
    {
        auto &reader = participant();
        if (!reader.depth) violation("unbalanced operation scope", nullptr, __FILE__, __LINE__);
        if (--reader.depth != 0) return;
        {
            auto &state = domain();
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!reader.staged.empty())
            {
                if (state.epoch == std::numeric_limits<uint64_t>::max())
                    violation("epoch overflow", nullptr, __FILE__, __LINE__);
                // The grace period starts after the operation finishes all
                // unlinking, even when a helper requests retirement earlier.
                const auto retiredEpoch = state.epoch++;
                for (auto entry : reader.staged)
                {
                    // A helper may retire storage before its caller finishes
                    // changing Guide slots. Final unpublication precedes the
                    // grace-period boundary, under the entry/retirement lock.
                    unpublishGuides(entry.pointer, entry.bytes);
                    entry.epoch = retiredEpoch;
                    state.retired.push_back(entry);
                }
                reader.staged.clear();
            }
            reader.epoch = 0;
        }
        collect();
    }

    void retire(void *pointer, size_t bytes, Deleter deleter, uintptr_t context,
                const char *file, int line)
    {
        if (!pointer) return;
        validateAddress(pointer, file, line);
        if (!deleter)
            violation("invalid retirement pointer/deleter", pointer, file, line);
        auto &state = domain();
        auto &reader = participant();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.pending_addresses.insert(pointer).second)
                violation("duplicate retirement", pointer, file, line);
            unpublishGuides(pointer, bytes);
            if (reader.depth)
                reader.staged.push_back({pointer, bytes, deleter, context, 0, file, line});
            else
            {
                if (state.epoch == std::numeric_limits<uint64_t>::max())
                    violation("epoch overflow", pointer, file, line);
                state.retired.push_back({pointer, bytes, deleter, context, state.epoch++, file, line});
            }
            ++state.outstanding;
        }
        if (!participant().depth) collect();
    }

    void collect()
    {
        auto &reader = participant();
        if (reader.collecting) return;
        reader.collecting = true;
        // Deleters may retire children. Revisit the queue after each batch.
        for (unsigned batch = 0; batch < 8; ++batch)
        {
            std::vector<Retired> ready;
            {
                auto &state = domain();
                std::unique_lock<std::mutex> lock(state.mutex, std::try_to_lock);
                if (!lock.owns_lock()) break;
                const auto oldest = oldestReader(state);
                while (!state.retired.empty() && state.retired.front().epoch < oldest && ready.size() < 256)
                {
                    const auto entry = state.retired.front();
                    // This check stays enabled in release builds.
                    for (const auto *active : state.participants)
                        if (active->epoch && active->epoch <= entry.epoch)
                            violation("free overlaps an active reader", entry.pointer, entry.file, entry.line);
                    ready.push_back(entry);
                    state.pending_addresses.erase(entry.pointer);
                    state.retired.pop_front();
                }
            }
            if (ready.empty()) break;
            for (const auto &entry : ready)
            {
                entry.deleter(entry.pointer, entry.context);
                auto &state = domain();
                std::lock_guard<std::mutex> lock(state.mutex);
                --state.outstanding;
            }
        }
        reader.collecting = false;
    }

    void retireJemalloc(void *pointer, const char *file, int line)
    {
        if (!pointer) return;
        validateAddress(pointer, file, line);
        retire(pointer, jem_sallocx(pointer, 0),
               [](void *p, uintptr_t) { jem_free(p); }, 0, file, line);
    }

    void retireJemallocArena(void *pointer, int flags, const char *file, int line)
    {
        if (!pointer) return;
        validateAddress(pointer, file, line);
        retire(pointer, jem_sallocx(pointer, 0),
               [](void *p, uintptr_t f) { jem_dallocx(p, static_cast<int>(f)); },
               static_cast<uintptr_t>(flags), file, line);
    }

    size_t pending()
    {
        auto &state = domain();
        std::lock_guard<std::mutex> lock(state.mutex);
        return state.outstanding;
    }

    void drain()
    {
        {
            auto &state = domain();
            std::lock_guard<std::mutex> lock(state.mutex);
            if (oldestReader(state) != std::numeric_limits<uint64_t>::max())
                violation("drain requested while readers are active", nullptr, __FILE__, __LINE__);
        }
        while (pending()) collect();
    }
}
