#include "zsim_malloc.h"

void* malloc_zsim(size_t size, DataType Type)
{
    void* Tmp;
    Tmp = malloc(size);
    zsim_allocate_approximate(Tmp, size, Type);
    return Tmp;
}

// void* malloc_zsim(void *ptr, size_t size, DataType Type)
// {
//     void* Tmp;
//     zsim_deallocate_approximate(ptr);
//     Tmp = realloc(ptr, size);
//     zsim_allocate_approximate(Tmp, size, Type);
//     return Tmp;
// }

void* calloc_zsim(size_t size1, size_t size2, DataType Type)
{
    void* Tmp;
    Tmp = calloc(size1, size2);
    zsim_allocate_approximate(Tmp, size1*size2, Type);
    return Tmp;
}

void free_zsim(void* ptr)
{
    zsim_deallocate_approximate(ptr);
    free(ptr);
}