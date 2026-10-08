#include "ProcessCommand.h"
#include "spdlog/spdlog.h"
#include <sstream>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

bool KeyValueStore::set(const std::string &key, const std::string &value)
{
    int ret = theDict.insert((void *)key.c_str(), key.size() + 1, (void *)value.c_str(), value.size() + 1);
    if (ret == 0)
    {
        spdlog::info("Failed to add or update key: {} in dictionary", key);
        return 0;
    }
    return 1;
}

std::string KeyValueStore::get(const std::string &key)
{
    // Use searchCopy, not search: search() hands back a raw pointer whose
    // TAG/ATC protection ends when the operation returns -- copying it here
    // would race with migration freeing the old object. searchCopy performs
    // the copy inside the data structure's instrumented public operation.
    std::string value;
    if (theDict.searchCopy((void *)key.c_str(), key.size() + 1, value))
    {
        spdlog::trace("GET {}", key);
        return value;
    }
    return ""; // Return empty string if key not found
}

std::string KeyValueStore::scan(const std::string &start_key, int n)
{
    // Length-prefixed pairs: values contain spaces, so the client parses by
    // byte offsets, not tokens. Values never contain '\n' (the framing
    // delimiter); the store is line-framed end to end.
    std::vector<std::pair<std::string, std::string>> pairs;
    int got = theDict.scanCopy((void *)start_key.c_str(), start_key.size() + 1, n, pairs);
    if (got < 0)
    {
        return "ERR unsupported"; // unordered structure
    }
    std::string resp = "OK " + std::to_string(got);
    for (const auto &kv : pairs)
    {
        resp += " " + std::to_string(kv.first.size()) + " " + std::to_string(kv.second.size()) +
                " " + kv.first + " " + kv.second;
    }
    return resp;
}

bool KeyValueStore::del(const std::string &key)
{
    spdlog::trace("DEL {}", key);

    void *res = theDict.remove((void *)key.c_str(), key.size() + 1);
    if (res)
    {
        jem_free(res);
        return 1;
    }
    return 0;
}

std::string ProcessCommand::execute(const std::string &command)
{
    std::istringstream iss(command);
    std::string cmd, modeArg, filePath;
    iss >> cmd;

    if (cmd == "SET" || cmd == "set")
    {
        std::string key, value;
        std::getline(iss >> std::ws, key, ' ');
        std::getline(iss >> std::ws, value);
        if (store.set(key, value))
        {
            spdlog::trace("SET {}", key);
            return "OK";
        }
        else
            return "ERR";
    }
    else if (cmd == "GET" || cmd == "get")
    {
        std::string key;
        if (iss >> key)
        {
            std::string value = store.get(key);
            if (!value.empty())
            {
                return value;
            }
            return "ERR"; // Key not found
        }
    }
    else if (cmd == "DEL" || cmd == "del")
    {
        std::string key;
        if (iss >> key && store.del(key))
        {
            return "OK";
        }
        return "ERR";
    }
    else if (cmd == "SCAN" || cmd == "scan")
    {
        std::string key;
        int n = 0;
        if (iss >> key >> n && n > 0)
        {
            // Cap the response size (10k pairs x ~1KB values ~ 10MB)
            if (n > 10000)
            {
                n = 10000;
            }
            return store.scan(key, n);
        }
        return "ERR";
    }
    else if (cmd == "STATS" || cmd == "stats")
    {
        return "OK";
    }
    else if (cmd == "OBASE" || cmd == "obase")
    {
#if CREST_RAW_POINTERS
        return "ERR OBASE disabled in raw-pointer build";
#else
        // Enable/disable tracking and migration 
        std::string mode;
        iss >> mode;
        if (mode == "none")
        {
            store.ObaseRTMode.store(0, std::memory_order_release);
            return "OK";
        }
        else if (mode == "decay")
        {
            store.ObaseRTMode.store(1, std::memory_order_release);
            if (!store.scanAndMigrator)
            {
                // Interval to scan soda and reset accessed bits.
                // Default 120s (YCSB); metabench used 720, meta traces 960.
                // Override with CREST_SCAN_INTERVAL_S for quick tests.
                uint64_t intervalDuration = 120;
                if (const char *env = getenv("CREST_SCAN_INTERVAL_S"))
                {
                    long v = atol(env);
                    if (v >= 1)
                    {
                        intervalDuration = (uint64_t)v;
                        spdlog::info("Scan interval overridden to {}s via CREST_SCAN_INTERVAL_S", intervalDuration);
                    }
                }
                // If obj not accessed for 3 consecutive intervals, demote it
                static const uint8_t inactiveWindowThreshold = 3;

                ObjectCollector *migrator = ObjectCollector::initializeIfNeeded(
                    intervalDuration,
                    inactiveWindowThreshold,
                    &store.theDict,
                    &store.sama,
                    &store.ObaseRTMode);

                if (migrator)
                {
                    store.scanAndMigrator.reset(migrator);
                }
            }
            return "OK";
        }
        else if (mode == "migrate")
        {
            store.ObaseRTMode.store(2, std::memory_order_release);
            return "OK";
        }
        return "ERR";
#endif
    }
    else if (cmd == "\n" || cmd == "")
    {
        return "OK";
    }

    return "ERR"; // Default response for unknown commands
}
