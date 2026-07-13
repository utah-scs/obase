#include "Guide.hpp"

extern "C"
{
    __attribute__((always_inline)) ActiveScopeGuard *createTAG()
    {
        return new ActiveScopeGuard();
    }

    __attribute__((always_inline)) void destroyTAG(ActiveScopeGuard *guard)
    {
        delete guard;
    }

    __attribute__((always_inline)) void addToTAG(ActiveScopeGuard *guard, void *varPtr)
    {
        ScopeGuardState state = g_scope_guard_state.load(std::memory_order_acquire);

        // Track in both PREPARE and ACTIVE. Tracking must begin in PREPARE:
        // the OC's convergence check only guarantees that every running
        // operation started after the epoch bump -- if those operations did
        // not track their dereferences from the start, the OC would enter
        // ACTIVE and migrate objects they still hold raw pointers to.
        if (state == ScopeGuardState::INACTIVE)
        {
            return;
        }

        // Track pointers and increment reference counts
        bool addResult;
        if (guard != nullptr)
        {
            // Public function with valid guard
            addResult = guard->add(varPtr);
        }
        else
        {
            // Private function - use thread-local storage directly
            addResult = ActiveScopeGuard::getThreadLocalSet()->insert(varPtr);
        }

        // Increment reference count if this is the first time we're seeing this pointer
        if (addResult)
        {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            reinterpret_cast<Guide<void> *>(varPtr)->incrementATC();
            __atomic_thread_fence(__ATOMIC_RELEASE);
        }
    }
}
