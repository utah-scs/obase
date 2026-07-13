#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <Size_in_GB>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    double gb = atof(argv[1]);
    size_t size = (size_t)(gb * 1024 * 1024 * 1024);

    printf("Allocating %.2f GB of Anonymous Memory...\n", gb);

    // 1. Use MAP_ANONYMOUS (Keep it in RAM, no disk file)
    // MAP_POPULATE prefaults the page tables
    char *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

    if (ptr == MAP_FAILED) {
        perror("mmap failed");
        exit(EXIT_FAILURE);
    }

    printf("Touching memory to ensure physical allocation...\n");
    // 2. Write to pages to ensure they are physically allocated (Dirty them)
    // Accessing every 4KB page
    size_t page_size = 4096;
    for (size_t i = 0; i < size; i += page_size) {
        ptr[i] = 0; 
    }

    printf("Locking memory to prevent TPP demotion...\n");
    // 3. Lock the memory (mlock)
    // This is CRITICAL. It prevents TPP from moving this cold memory to CXL.
    // It forces the kernel to move your *workload* instead.
    if (mlock(ptr, size) != 0) {
        perror("mlock failed (Are you root? check ulimit -l)");
        // If we can't lock, we must exit, otherwise TPP will just move us.
        exit(EXIT_FAILURE); 
    }

    printf("Memory allocated and LOCKED in DRAM. Press Ctrl+C to free.\n");
    
    // 4. Sleep forever (Low CPU usage)
    while (1) {
        sleep(10);
    }

    return 0;
}