// Validator fixture: indexing into a managed object's address.
// Expected: [obase-validate:arithmetic], nonzero exit.
#include "Guide.hpp"

struct GNode
{
    Guide<void> val;
};

char nthByte(GNode *n, int i)
{
    return ((char *)(void *)n->val)[i];
}
