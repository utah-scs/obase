#ifndef OBASE_TEST_BASIC_H
#define OBASE_TEST_BASIC_H

#include <cstddef>
#include "GuideAnnotations.h"

struct Item
{
    OBASE_GUIDED void *payload;
    OBASE_GUIDED char *name;
    size_t payload_len;
    Item *next;
};

#endif
