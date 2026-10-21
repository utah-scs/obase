#pragma once

#include <cstddef>
#include <cstdint>

// Shared-object lifetime protection, independent of OBASE instrumentation.
// An owner must unlink an object before retiring it. A borrowed pointer may
// only be used inside a Scope that began before the pointer was acquired.
namespace reclamation
{
    class Scope
    {
    public:
        Scope();
        ~Scope();
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };

    using Deleter = void (*)(void *, uintptr_t);
    void retire(void *pointer, size_t bytes, Deleter deleter, uintptr_t context,
                const char *file, int line);
    void retireJemalloc(void *pointer, const char *file, int line);
    void retireJemallocArena(void *pointer, int flags, const char *file, int line);
    void collect();
    // Shutdown/test boundary: requires all operation scopes to have exited.
    void drain();
    size_t pending();

    template <typename T>
    void retireDelete(T *pointer, const char *file, int line)
    {
        retire(pointer, sizeof(T), [](void *p, uintptr_t) { delete static_cast<T *>(p); },
               0, file, line);
    }

    template <typename T>
    void retireDeleteArray(T *pointer, const char *file, int line)
    {
        // This helper is for raw arrays without embedded Guide slots.
        retire(pointer, 0, [](void *p, uintptr_t) { delete[] static_cast<T *>(p); },
               0, file, line);
    }
}

#define CREST_RETIRE_DELETE(pointer) ::reclamation::retireDelete((pointer), __FILE__, __LINE__)
#define CREST_RETIRE_DELETE_ARRAY(pointer) ::reclamation::retireDeleteArray((pointer), __FILE__, __LINE__)
#define CREST_RETIRE_FREE(pointer) ::reclamation::retireJemalloc((pointer), __FILE__, __LINE__)
