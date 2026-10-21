#include "Guide.hpp"

struct ReclaimedValue
{
    Guide<void> data;
    ~ReclaimedValue() { data.destroy(); }
};

extern void consume(void *);

void read_reclaimed_value(ReclaimedValue *value)
{
    consume(value->data);
}

void retire_reclaimed_value(ReclaimedValue *value)
{
    CREST_RETIRE_DELETE(value);
}
