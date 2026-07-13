// Validator fixture: raw aliases of a guide slot.
// Expected: two [obase-validate:slot-alias] errors, nonzero exit.
#include "Guide.hpp"
#include <cstdint>

struct GNode
{
    Guide<void> val;
};

void **aliasSlot(GNode *n)
{
    return (void **)&n->val;
}

uintptr_t *slotWordPtr(GNode *n)
{
    return (uintptr_t *)&n->val;
}

// Allowed: an integer cast of the slot's address is index arithmetic (how
// SODA slot numbers are computed); there is no store path through it.
uintptr_t slotIndex(GNode *n)
{
    return (uintptr_t)&n->val;
}
