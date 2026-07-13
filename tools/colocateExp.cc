#include <iostream>
#include <vector>
#include <random>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/perf_event.h>
#include <asm/unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <cstdlib>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <set>
#include <chrono>
#include <linux/mman.h>

/*
A simple benchmark to illustrate the benefits of colocation of hot data.
*/

// const int page_size = 4096; // Assuming 4KB pages
// const size_t page_size = 2097152; // Assuming 2MB pages

class Object
{
public:
    char *data;
    size_t size;

    Object(size_t s) : size(s) {}
    ~Object() {}

    __attribute__((noinline)) volatile unsigned char dummy_function(volatile unsigned char c)
    {
        return c;
    }

    void access()
    {
        volatile unsigned char sum = 0;
        for (size_t i = 0; i < size; ++i)
        {
            sum ^= dummy_function(data[i]);
        }
    }
};

class Experiment
{
private:
    std::vector<Object *> objects;
    std::vector<int> hotIndices;
    size_t numObjects;
    size_t objectSize;
    double hotPercentage;
    size_t numOperations;
    bool clustered;
    char *clusteredBlock;
    size_t l3_cache_size;
    size_t tlb_entries;
    bool realisticAllocation;
    std::set<size_t> hotPages;
    int page_size;

    std::mt19937 gen;
    std::uniform_int_distribution<> hotDis;
    std::uniform_int_distribution<> pageDis;

    static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                                int cpu, int group_fd, unsigned long flags)
    {
        return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
    }
    void get_system_info()
    {
        // Get L3 cache size
        std::ifstream cache_file("/sys/devices/system/cpu/cpu0/cache/index3/size");
        std::string cache_size_str;
        if (cache_file >> cache_size_str)
        {
            l3_cache_size = std::stoull(cache_size_str);
            if (cache_size_str.back() == 'K')
                l3_cache_size *= 1024;
            if (cache_size_str.back() == 'M')
                l3_cache_size *= 1024 * 1024;
        }
        else
        {
            l3_cache_size = 8 * 1024 * 1024; // Assume 8MB if unable to read
        }

        // Set TLB entries based on the L2 TLB size
        if (page_size == 4096)
        {
            tlb_entries = 1536; // L2 TLB size for 4K/2M pages
        }
        else if (page_size == 2097152)
        {
            tlb_entries = 32; // L2 TLB size for 2M pages
        }
        else if (page_size == 1073741824)
        {
            tlb_entries = 4; // L2 TLB size for 1G pages
        }
        else
        {
            std::cerr << "Invalid page size" << std::endl;
            exit(1);
        }
    }
    void allocateObjectsRealistic()
    {
        char *memoryBlock;
        if (page_size == 4096)
        {
            // char *memoryBlock = new char[numObjects * objectSize];
            // mmap the block
            memoryBlock = static_cast<char *>(mmap(nullptr, numObjects * objectSize, PROT_READ | PROT_WRITE,
                                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        }
        else if (page_size == 2097152)
        {
            memoryBlock = static_cast<char *>(mmap(nullptr, numObjects * objectSize, PROT_READ | PROT_WRITE,
                                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0));
        }
        else if (page_size == 1073741824)
        {
            memoryBlock = static_cast<char *>(mmap(nullptr, numObjects * objectSize, PROT_READ | PROT_WRITE,
                                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB, -1, 0));
        }
        else
        {
            std::cerr << "Invalid page size" << std::endl;
            exit(1);
        }

        if (memoryBlock == MAP_FAILED)
        {
            perror("mmap with hugepages failed");
            exit(1);
        }
        for (size_t i = 0; i < numObjects; ++i)
        {
            objects.push_back(new Object(objectSize));
            objects.back()->data = memoryBlock + i * objectSize;
            for (size_t j = 0; j < objectSize; ++j)
            {
                objects.back()->data[j] = static_cast<char>((i + j) % 256);
            }
        }
    }

    void selectHotObjectsRealistic()
    {
        size_t hotCount = static_cast<size_t>(numObjects * hotPercentage);
        if (clustered)
        {
            // Select a contiguous block of hot objects, starting at a page boundary
            size_t objectsPerPage = page_size / objectSize;
            size_t pagesNeeded = (hotCount + objectsPerPage - 1) / objectsPerPage;
            size_t totalPages = (numObjects + objectsPerPage - 1) / objectsPerPage;

            // Ensure we don't start beyond the last possible page that can fit the hot set
            size_t maxStartPage = totalPages - pagesNeeded;
            size_t startPage = gen() % (maxStartPage + 1);
            size_t startIndex = startPage * objectsPerPage;

            hotIndices.resize(hotCount);
            for (size_t i = 0; i < hotCount; ++i)
            {
                hotIndices[i] = startIndex + i;
            }
        }
        else
        {
            // Select random objects
            // Each object has an equal chance of being selected.
            hotIndices.resize(hotCount);
            std::vector<size_t> allIndices(numObjects);
            std::iota(allIndices.begin(), allIndices.end(), 0);
            std::shuffle(allIndices.begin(), allIndices.end(), gen);
            std::copy(allIndices.begin(), allIndices.begin() + hotCount, hotIndices.begin());
            std::sort(hotIndices.begin(), hotIndices.end());
        }
    }

public:
    Experiment(size_t nObj, size_t objSize, double hotPct, size_t nOps, bool clust, bool realistic, int page_size)
        : numObjects(nObj), objectSize(objSize), hotPercentage(hotPct),
          numOperations(nOps), clustered(clust), clusteredBlock(nullptr),
          gen(std::random_device{}()),
          hotDis(0, static_cast<int>(nObj * hotPct) - 1),
          pageDis(0, page_size - objectSize),
          realisticAllocation(realistic),
          page_size(page_size)
    {
        if (!realisticAllocation)
        {
            /*
                These methods creates a setup where on "spread" layout, each objects are allocated
                on a separate page at a random offset. On "clustered" layout, all objects are allocated on a single page.
            */
            allocateObjects();
            selectHotObjects();
            selectHotObjectsSequential();
        }
        else
        {
            /*
                The following methods creates a setup where all the objects are packed together in a single memory block.
                This is more realistic as it simulates a real-world scenario where objects are allocated in a contiguous memory block.
                On "spread" selection method, a random subset of objects are selected to be hot.
                On "clustered" selection method, a contiguous block of objects are selected to be hot.
            */
            allocateObjectsRealistic();
            selectHotObjectsRealistic();
        }
    }

    ~Experiment()
    {

        // Clear the vector
        objects.clear();
    }

    void allocateObjects()
    {
        if (clustered)
        {
            clusteredBlock = new char[numObjects * objectSize];
            for (size_t i = 0; i < numObjects; ++i)
            {
                objects.push_back(new Object(objectSize));
                objects.back()->data = clusteredBlock + i * objectSize;
                for (size_t j = 0; j < objectSize; ++j)
                {
                    objects.back()->data[j] = static_cast<char>((i + j) % 256);
                }
            }
        }
        else
        {
            for (size_t i = 0; i < numObjects; ++i)
            {
                char *page = static_cast<char *>(mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
                if (page == MAP_FAILED)
                {
                    std::cerr << "mmap failed" << std::endl;
                    exit(1);
                }

                // Generate a random offset within the page
                size_t offset = pageDis(gen);

                objects.push_back(new Object(objectSize));
                objects.back()->data = page + offset;

                for (size_t j = 0; j < objectSize; ++j)
                {
                    objects.back()->data[j] = static_cast<char>((i + j) % 256);
                }
            }
        }
    }

    void selectHotObjects()
    {
        size_t hotCount = static_cast<size_t>(numObjects * hotPercentage);
        hotIndices.resize(hotCount);
        std::iota(hotIndices.begin(), hotIndices.end(), 0);
        std::shuffle(hotIndices.begin(), hotIndices.end(), gen);
    }

    // a function that selects a subset of sequential objects to be hot
    void selectHotObjectsSequential()
    {
        size_t hotCount = static_cast<size_t>(numObjects * hotPercentage);
        hotIndices.resize(hotCount);
        std::iota(hotIndices.begin(), hotIndices.end(), 0);
    }

    void run()
    {
        get_system_info();
        struct perf_event_attr pe;
        long long count[6];
        int fd[6];

        memset(&pe, 0, sizeof(struct perf_event_attr));
        pe.type = PERF_TYPE_RAW;
        pe.size = sizeof(struct perf_event_attr);
        pe.disabled = 1;
        pe.exclude_kernel = 1;
        pe.exclude_hv = 1;

        /*
       L3 miss rate relative to all loads = mem_load_retired.l3_miss / mem_inst_retired.all_loads
       TLB Miss Rate = (mem_inst_retired.stlb_miss_loads + mem_inst_retired.stlb_miss_stores) /
       (mem_inst_retired.all_loads + mem_inst_retired.all_stores)
       */

        // Set up the events
        unsigned long long events[6] = {
            0x81d0, // mem_inst_retired.all_loads
            0x82d0, // mem_inst_retired.all_stores
            0x11d0, // mem_inst_retired.stlb_miss_loads
            0x12d0, // mem_inst_retired.stlb_miss_stores
            0x20d1, // mem_load_retired.l3_miss
            0x81d0  // mem_load_retired.all_loads (same as mem_inst_retired.all_loads)
        };

        for (int i = 0; i < 6; i++)
        {
            pe.config = events[i];
            fd[i] = perf_event_open(&pe, 0, -1, -1, 0);
            if (fd[i] == -1)
            {
                std::cerr << "Error opening event " << std::hex << events[i] << std::endl;
                exit(1);
            }
        }

        for (int i = 0; i < 6; i++)
        {
            ioctl(fd[i], PERF_EVENT_IOC_RESET, 0);
            ioctl(fd[i], PERF_EVENT_IOC_ENABLE, 0);
        }

        // start time us
        auto start = std::chrono::high_resolution_clock::now();

        // random access pattern
        for (size_t i = 0; i < numOperations; ++i)
        {
            int index = hotIndices[hotDis(gen)];
            objects[index]->access();
        }

        // end time us
        auto end = std::chrono::high_resolution_clock::now();

        // sequential access pattern
        // for (size_t i = 0; i < numOperations; ++i)
        // {
        //     int index = hotIndices[i % hotIndices.size()];
        //     objects[index]->access();
        // }

        for (int i = 0; i < 6; i++)
        {
            ioctl(fd[i], PERF_EVENT_IOC_DISABLE, 0);
            read(fd[i], &count[i], sizeof(long long));
            close(fd[i]);
        }

        double tlb_miss_rate = (double)(count[2] + count[3]) / (count[0] + count[1]);
        double l3_miss_rate = (double)count[4] / count[5];

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "L3 Cache Size: " << l3_cache_size / (1024 * 1024) << " MiB" << std::endl;
        std::cout << "TLB Entries: " << tlb_entries << " coverage: " << (tlb_entries * page_size) / (1024 * 1024) << " MiB" << std::endl;
        std::cout << "Total loads: " << count[0] << std::endl;
        std::cout << "Total stores: " << count[1] << std::endl;
        std::cout << "STLB miss loads: " << count[2] << std::endl;
        std::cout << "STLB miss stores: " << count[3] << std::endl;
        std::cout << "L3 misses: " << count[4] << std::endl;
        std::cout << "TLB miss rate: " << (tlb_miss_rate * 100) << "%" << std::endl;
        std::cout << "L3 cache miss rate: " << (l3_miss_rate * 100) << "%" << std::endl;
        std::cout << "Accesses/sec: " << numOperations / std::chrono::duration<double>(end - start).count() << std::endl;
    }
    void calculateHotPages()
    {
        hotPages.clear();
        size_t objectsPerPage = page_size / objectSize;
        for (size_t index : hotIndices)
        {
            hotPages.insert(index / objectsPerPage);
        }
    }

    void printPageUsage()
    {
        size_t totalPages = (numObjects * objectSize + page_size - 1) / page_size;
        size_t objectsPerPage = page_size / objectSize;

        if (realisticAllocation)
        {
            calculateHotPages();
            std::cout << "Total pages used: " << totalPages << std::endl;
            std::cout << "Hot pages touched: " << hotPages.size() << std::endl;
        }
        else
        {
            if (clustered)
            {
                size_t hotObjectPages = (static_cast<size_t>(numObjects * hotPercentage) * objectSize + page_size - 1) / page_size;
                std::cout << "Total pages used: " << totalPages << std::endl;
                std::cout << "Hot pages used: " << hotObjectPages << std::endl;
            }
            else
            {
                std::cout << "Total pages used: " << numObjects << std::endl;
                std::cout << "Hot pages used: " << static_cast<size_t>(numObjects * hotPercentage) << std::endl;
            }
        }
    }
};

int main(int argc, char *argv[])
{
    if (argc != 8)
    {
        std::cerr << "Usage: " << argv[0] << " <num_objects> <object_size> <hot_percentage> <num_operations> <clustered> <realistic> <sys page size>" << std::endl;
        return 1;
    }

    size_t numObjects = std::stoull(argv[1]);
    size_t objectSize = std::stoull(argv[2]);
    double hotPercentage = std::stod(argv[3]);
    size_t numOperations = std::stoull(argv[4]);
    bool clustered = std::stoi(argv[5]) != 0;
    bool realistic = std::stoi(argv[6]) != 0;
    int page_size = std::stoi(argv[7]);

    std::cout << "Running experiment with:" << std::endl
              << "Number of objects: " << numObjects << std::endl
              << "Object size: " << objectSize << " bytes" << std::endl
              << "Number of operations: " << numOperations << std::endl
              << "Hot percentage: " << (hotPercentage * 100) << "%" << std::endl
              << "Total data size: " << (numObjects * objectSize) / (1024 * 1024) << " MiB" << std::endl
              << "Hot data size: " << ((numObjects * hotPercentage) * objectSize) / (1024 * 1024) << " MiB" << std::endl
              << "Layout: " << (clustered ? "Clustered" : "Spread") << std::endl;

    Experiment exp(numObjects, objectSize, hotPercentage, numOperations, clustered, realistic, page_size);
    exp.run();
    exp.printPageUsage();

    return 0;
}