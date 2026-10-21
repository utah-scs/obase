#include "ProcessCommand.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

static void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << CREST_DS_NAME << ": " << message << std::endl;
        std::abort();
    }
}

int main()
{
    spdlog::set_level(spdlog::level::warn);
    {
        KeyValueStore store;
        std::string key = "retained-value";
        const std::string original(4096, 'A'), replacement(8192, 'B');
        require(store.set(key, original), "initial insert failed");
        {
            // Exercise the dictionary API directly: the server wrapper must
            // not be the only thing protecting a borrowed pointer.
            reclamation::Scope reader;
            auto *old = static_cast<const char *>(store.theDict.search(key.data(), key.size() + 1));
            require(old != nullptr, "initial search failed");
            std::thread updater([&] {
                for (int i = 0; i < 128; ++i)
                    require(store.set(key, replacement), "replacement failed");
            });
            updater.join();
            require(reclamation::pending() > 0, "updates reclaimed storage while reader was active");
            require(std::memcmp(old, original.c_str(), original.size() + 1) == 0,
                    "borrowed value changed after replacement");

            auto *updated = static_cast<const char *>(store.theDict.search(key.data(), key.size() + 1));
            require(updated != nullptr, "replacement search failed");
            std::thread remover([&] {
                require(store.del(key), "remove failed");
                require(store.get(key).empty(), "removed key remains visible");
                for (int i = 0; i < 128; ++i)
                    require(store.set(key, original), "reinsertion failed");
            });
            remover.join();
            require(std::memcmp(updated, replacement.c_str(), replacement.size() + 1) == 0,
                    "borrowed value changed after removal/reinsertion");
            require(std::memcmp(old, original.c_str(), original.size() + 1) == 0,
                    "original borrowed value was reused");
        }
        reclamation::drain();
        require(reclamation::pending() == 0, "retirements did not drain");

        // Force multiple tree levels / hash buckets, then let destruction
        // verify that splits have not left duplicate owners of child storage.
        for (int i = 0; i < 128; ++i)
            require(store.set("split-key-" + std::to_string(i), original), "split insert failed");
        for (int i = 0; i < 128; ++i)
            require(store.get("split-key-" + std::to_string(i)) == original, "split lost a value");
    }
    require(reclamation::pending() == 0, "destruction left pending retirements");
    std::cout << CREST_DS_NAME << ": borrowed values survive concurrent update/delete; pending=0\n";
}
