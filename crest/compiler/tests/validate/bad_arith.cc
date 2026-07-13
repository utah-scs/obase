// Validator fixture: pointer arithmetic on a managed object's address.
// Expected: [obase-validate:arithmetic], nonzero exit.
#include "Guide.hpp"

struct GNode
{
    Guide<void> val;
};

char *pastHeader(GNode *n)
{
    return (char *)(void *)n->val + 8;
}
