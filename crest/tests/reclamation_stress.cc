#include "ProcessCommand.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char **argv)
{
    const int seconds = argc > 1 ? std::stoi(argv[1]) : 10;
    spdlog::set_level(spdlog::level::warn);
    KeyValueStore store;
    std::vector<std::string> keys;
    for (int i = 0; i < 32; ++i)
    {
        keys.push_back("Kq/ConcurrentKey" + std::to_string(i));
        store.set(keys.back(), std::string(4096, 'A'));
    }
    std::atomic<bool> go{false}, failed{false};
    std::atomic<uint64_t> reads{0}, writes{0};
    std::vector<std::thread> workers;
    for (int id = 0; id < 6; ++id)
    {
        workers.emplace_back([&, id] {
            while (!go.load()) std::this_thread::yield();
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            uint64_t iteration = id;
            while (!failed.load() && std::chrono::steady_clock::now() < end)
            {
                const auto &key = keys[(iteration++ * 13) % keys.size()];
                if (id < 2)
                {
                    const char ch = ((iteration / keys.size()) % 2) ? 'A' : 'B';
                    const size_t length = ch == 'A' ? 4096 : 8192;
                    if (!store.set(key, std::string(length, ch))) failed.store(true);
                    ++writes;
                }
                else
                {
                    auto value = store.get(key);
                    const bool a = value.size() == 4096 && value == std::string(4096, 'A');
                    const bool b = value.size() == 8192 && value == std::string(8192, 'B');
                    if (!a && !b)
                    {
                        failed.store(true);
                        std::cerr << "CORRUPTION key=" << key << " size=" << value.size() << '\n';
                    }
                    ++reads;
                }
            }
        });
    }
    go.store(true);
    for (auto &thread : workers) thread.join();
    reclamation::drain();
    std::cout << "reads=" << reads << " writes=" << writes << " failed=" << failed
              << " pending=" << reclamation::pending() << std::endl;
    return failed ? 1 : 0;
}
