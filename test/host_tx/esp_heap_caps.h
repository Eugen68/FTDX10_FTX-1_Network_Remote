/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdlib.h>
enum { MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2 };
static inline void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}
