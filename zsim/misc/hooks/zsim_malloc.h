#ifndef __ZSIM_MALLOC_H__
#define __ZSIM_MALLOC_H__

#include "zsim_hooks.h"
#include <malloc.h>
#include <stdio.h>

static void *zsim_malloc_hook (size_t size, const void *caller);
static void *zsim_realloc_hook (void *ptr, size_t size, const void *caller);
static void zsim_free_hook (void *ptr, const void *caller);

static void *(*OldMallocHook) (size_t, const void *);
static void *(*OldReallocHook) (void *, size_t, const void *);
static void (*OldFreeHook) (void *, const void *);

// DataType GlobalType;
// DataValue* GlobalMin;
// DataValue* GlobalMax;

void* malloc_zsim(size_t size, DataType Type);
// void* malloc_zsim(void *ptr, size_t size, DataType Type);
void* calloc_zsim(size_t size1, size_t size2, DataType Type);
void free_zsim(void* ptr);

// static void zsim_mallocs_init(DataType Type, DataValue* Min, DataValue* Max)
// {
//     printf("ZSIM Malloc Hooks Active\n");
//     OldMallocHook = __malloc_hook;
//     OldReallocHook = __realloc_hook;
//     OldFreeHook = __free_hook;
//     __malloc_hook = zsim_malloc_hook;
//     __realloc_hook = zsim_realloc_hook;
//     __free_hook = zsim_free_hook;
//     GlobalType = Type;
//     GlobalMin = Min;
//     GlobalMax = Max;
// }

// static void *zsim_malloc_hook (size_t size, const void *caller)
// {
//     void *Tmp;
//     __malloc_hook = OldMallocHook;
//     __realloc_hook = OldReallocHook;
//     __free_hook = OldFreeHook;
//     Tmp = malloc (size);
//     zsim_allocate_approximate(Tmp, size, GlobalType, GlobalMin, GlobalMax);
//     OldMallocHook = __malloc_hook;
//     OldReallocHook = __realloc_hook;
//     OldFreeHook = __free_hook;
//     __malloc_hook = zsim_malloc_hook;
//     __realloc_hook = zsim_realloc_hook;
//     __free_hook = zsim_free_hook;
//     return Tmp;
// }

// static void *zsim_realloc_hook (void *ptr, size_t size, const void *caller)
// {
//     void *Tmp;
//     __malloc_hook = OldMallocHook;
//     __realloc_hook = OldReallocHook;
//     __free_hook = OldFreeHook;
//     zsim_deallocate_approximate(ptr);
//     Tmp = realloc (ptr, size);
//     zsim_allocate_approximate(Tmp, size, GlobalType, GlobalMin, GlobalMax);
//     // zsim_reallocate_approximate(Tmp, size);
//     OldMallocHook = __malloc_hook;
//     OldReallocHook = __realloc_hook;
//     OldFreeHook = __free_hook;
//     __malloc_hook = zsim_malloc_hook;
//     __realloc_hook = zsim_realloc_hook;
//     __free_hook = zsim_free_hook;
//     return Tmp;
// }

// static void zsim_free_hook (void *ptr, const void *caller)
// {
//     __malloc_hook = OldMallocHook;
//     __realloc_hook = OldReallocHook;
//     __free_hook = OldFreeHook;
//     zsim_deallocate_approximate(ptr);
//     free(ptr);
//     OldMallocHook = __malloc_hook;
//     OldReallocHook = __realloc_hook;
//     OldFreeHook = __free_hook;
//     __malloc_hook = zsim_malloc_hook;
//     __realloc_hook = zsim_realloc_hook;
//     __free_hook = zsim_free_hook;
// }

#endif // __ZSIM_MALLOC_H__