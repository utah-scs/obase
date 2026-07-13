// Validator fixture: legal guide usage -- must produce zero violations.
#include "Guide.hpp"
#include <cstring>
#include <string>

struct GNode
{
    Guide<void> key;
    Guide<void> val;
    size_t val_len;
    GNode *next;
};

// Pin / publish / free: the canonical update pattern.
void update(GNode *n, void *fresh, size_t len)
{
    void *old = static_cast<void *>(n->val);
    MemType old_type = n->val.getMemType();
    n->val = fresh;
    n->val_len = len;
    g_sama->free(old, old_type);
}

// Reading the object through its pinned raw address.
int compare(GNode *n, const void *key, size_t len)
{
    return memcmp(n->key, key, len);
}

// Copying the value out inside the operation.
void copyOut(GNode *n, std::string &out)
{
    out.assign((const char *)(void *)n->val, n->val_len);
}

// Dereferencing one byte (no arithmetic).
char firstByte(GNode *n)
{
    return *(const char *)(void *)n->val;
}
