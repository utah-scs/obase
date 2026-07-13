#ifndef PROCESSCOMMAND_H
#define PROCESSCOMMAND_H

// Data structure selected at build time: make all DS=<name> (see dsconfig.h)
#include "dsconfig.h"

#include "ObjectCollector.h"
#include "Sama.h"
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <fstream>
#include <mutex>

class KeyValueStore
{
public:
    KeyValueStore()
        // : sama({MemType::DRAM})
        : sama({MemType::DRAM, MemType::DRAM_2MB_THP}) // COLD_HEAP, HOT_HEAP
    // : sama({MemType::DRAM, MemType::DRAM_2MB_HUGETLBFS}) // COLD_HEAP, HOT_HEAP
    // : sama({MemType::DRAM_1GB_HUGETLBFS})
    // : sama({MemType::DRAM, MemType::SSD})
    {
        g_sama = &sama;

        // needed for sim_inc_dict
        /*
        // Initialize the dictionary once here
        uint8_t hashseed[16];
        for (int i = 0; i < 16; i++)
        {
            hashseed[i] = i;
        }
        dictSetHashFunctionSeed(hashseed);
        theDict = dictCreate();
        */

        ObaseRTMode.store(0, std::memory_order_release);
        spdlog::info("Data structure: {}", CREST_DS_NAME);
        spdlog::info("OBASE runtime mode: {}", ObaseRTMode.load());
    }

    ~KeyValueStore()
    {
        // needed for sim_inc_dict
        // if (theDict)
        // {
        //     dictRelease(theDict);
        // }
    }

    bool set(const std::string &key, const std::string &value);
    std::string get(const std::string &key);
    bool del(const std::string &key);
    // Range scan: up to n key/value pairs from the first key >= start_key.
    // Returns the full wire response ("OK <count> <klen> <vlen> <key> <value>..."
    // or "ERR unsupported" for unordered structures).
    std::string scan(const std::string &start_key, int n);

    /* OBASE runtime status
     *  0    none,   // no migration or decay
     *  1    decay,  // decay object's access activity in soda
     *  2    migrate // enable migration
     * Written by client-command threads, read by the migrator thread.
     */
    std::atomic<uint8_t> ObaseRTMode;

    dict theDict;

    std::unique_ptr<ObjectCollector> scanAndMigrator;
    Sama sama;

private:
    bool profilingEnabled = false;
};

class ProcessCommand
{
public:
    std::string execute(const std::string &command);

private:
    KeyValueStore store;
};

#endif // PROCESSCOMMAND_H
