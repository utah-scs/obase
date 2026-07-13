#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

size_t getCurrentRSS(pid_t pid)
{
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror("smaps fopen");
        return 0;
    }
    
    size_t rss = 0;
    size_t hugetlb = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        if (strncmp(line, "Rss:", 4) == 0)
        {
            size_t temp_rss = 0;
            sscanf(line, "Rss: %zu kB", &temp_rss);
            rss += temp_rss;
        }
        // Track both Private and Shared HugetlbPages
        else if (strncmp(line, "Private_Hugetlb:", 15) == 0)
        {
            size_t temp_huge = 0;
            sscanf(line, "Private_Hugetlb: %zu kB", &temp_huge);
            hugetlb += temp_huge;
        }
        else if (strncmp(line, "Shared_Hugetlb:", 14) == 0)
        {
            size_t temp_huge = 0;
            sscanf(line, "Shared_Hugetlb: %zu kB", &temp_huge);
            hugetlb += temp_huge;
        }
        // For AnonHugePages (transparent huge pages)
        else if (strncmp(line, "AnonHugePages:", 13) == 0)
        {
            size_t temp_huge = 0;
            sscanf(line, "AnonHugePages: %zu kB", &temp_huge);
            hugetlb += temp_huge;
        }
    }
    
    fclose(fp);
    printf("Regular RSS: %zu kB, HugeTLB pages: %zu kB\n", rss, hugetlb);
    return ((rss + hugetlb) * 1024)/ 4096;
}

void get_rss(pid_t pid, unsigned long *rss) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    if (fscanf(file, "%*s %lu", rss) != 1) {
        perror("fscanf");
        fclose(file);
        exit(EXIT_FAILURE);
    }

    fclose(file);
}

void write_rss_to_file(const char *filename, unsigned long rss, int append) {
    FILE *file;
    if (append) {
        file = fopen(filename, "a");
    } else {
        file = fopen(filename, "w");
    }

    if (file == NULL) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    // Convert pages to KiB: pages * page size in bytes / 1024 (to convert bytes to KiB)
    unsigned long rss_kib = rss * (unsigned long)sysconf(_SC_PAGESIZE) / 1024;

    fprintf(file, "%lu\n", rss_kib);
    fclose(file);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <interval in seconds> <pid> <filename>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int interval = atoi(argv[1]);
    pid_t pid = (pid_t)atoi(argv[2]);
    const char *filename = argv[3];

    int first_time = 1;

    while (1) {
        unsigned long rss;
        // get_rss(pid, &rss);
        rss = getCurrentRSS(pid);
        write_rss_to_file(filename, rss, !first_time);
        first_time = 0;
        sleep(interval);
    }

    return 0;
}
