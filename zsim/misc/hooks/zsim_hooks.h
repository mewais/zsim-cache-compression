#ifndef __ZSIM_HOOKS_H__
#define __ZSIM_HOOKS_H__

#include <stdint.h>
#include <stdio.h>

//Avoid optimizing compilers moving code around this barrier
#define COMPILER_BARRIER() { __asm__ __volatile__("" ::: "memory");}

//These need to be in sync with the simulator
#define ZSIM_MAGIC_OP_ROI_BEGIN         (1025)
#define ZSIM_MAGIC_OP_ROI_END           (1026)
#define ZSIM_MAGIC_OP_REGISTER_THREAD   (1027)
#define ZSIM_MAGIC_OP_HEARTBEAT         (1028)
#define ZSIM_MAGIC_OP_WORK_BEGIN        (1029) //ubik
#define ZSIM_MAGIC_OP_WORK_END          (1030) //ubik

#ifdef __x86_64__
#define HOOKS_STR  "HOOKS"
static inline void zsim_magic_op(uint64_t op) {
    COMPILER_BARRIER();
    __asm__ __volatile__("xchg %%rcx, %%rcx;" : : "c"(op));
    COMPILER_BARRIER();
}
#else
#define HOOKS_STR  "NOP-HOOKS"
static inline void zsim_magic_op(uint64_t op) {
    //NOP
}
#endif

static inline void zsim_roi_begin() {
    printf("[" HOOKS_STR "] ROI begin\n");
    zsim_magic_op(ZSIM_MAGIC_OP_ROI_BEGIN);
}

static inline void zsim_roi_end() {
    zsim_magic_op(ZSIM_MAGIC_OP_ROI_END);
    printf("[" HOOKS_STR  "] ROI end\n");
}

static inline void zsim_heartbeat() {
    zsim_magic_op(ZSIM_MAGIC_OP_HEARTBEAT);
}

static inline void zsim_work_begin() { zsim_magic_op(ZSIM_MAGIC_OP_WORK_BEGIN); }
static inline void zsim_work_end() { zsim_magic_op(ZSIM_MAGIC_OP_WORK_END); }

typedef enum {
    HOOKS_UINT8,
    HOOKS_INT8,
    HOOKS_UINT16,
    HOOKS_INT16,
    HOOKS_UINT32,
    HOOKS_INT32,
    HOOKS_UINT64,
    HOOKS_INT64,
    HOOKS_FLOAT,
    HOOKS_DOUBLE
} DataType;

union DataValue
{
    uint8_t HOOKS_UINT8;
    int8_t HOOKS_INT8;
    uint16_t HOOKS_UINT16;
    int16_t HOOKS_INT16;
    uint32_t HOOKS_UINT32;
    int32_t HOOKS_INT32;
    uint64_t HOOKS_UINT64;
    int64_t HOOKS_INT64;
    float HOOKS_FLOAT;
    double HOOKS_DOUBLE;
};

static inline void zsim_allocate_approximate(void* Start, uint64_t ByteLength, DataType Type, DataValue* Min, DataValue* Max)
{
    printf("[" HOOKS_STR "] Approximate Allocation\n");
    __asm__ __volatile__
    (
        ".byte 0x0F, 0x1F, 0x80, 0xFF, 0x00, 0x11, 0x22 ;\n\t"
        "add %5, %0 ;\n\t"
        "add %5, %1 ;\n\t"
        "add %5, %2 ;\n\t"
        "add %5, %3 ;\n\t"
        "add %5, %4 ;\n\t"
        :
        : "r" ((uint64_t)Start), "r" (ByteLength), "r" (Type), "r" ((uint64_t)Min), "r" ((uint64_t)Max), "i" (0)
        :
    );
}

static inline void zsim_reallocate_approximate(void* Start, uint64_t ByteLength)
{
    printf("[" HOOKS_STR "] Approximate Reallocation\n");
    __asm__ __volatile__
    (
        ".byte 0x0F, 0x1F, 0x80, 0xFF, 0x00, 0x11, 0x33 ;\n\t"
        "add %2, %0 ;\n\t"
        "add %2, %1 ;\n\t"
        "add %2"
        :
        : "r" ((uint64_t)Start), "r" (ByteLength), "i" (0)
        :
    );
}

static inline void zsim_deallocate_approximate(void* Start)
{
    printf("[" HOOKS_STR "] Approximate Deallocation\n");
    __asm__ __volatile__
    (
        ".byte 0x0F, 0x1F, 0x80, 0xFF, 0x00, 0x11, 0x44 ;\n\t"
        "add %1, %0 ;\n\t"
        "add %1, %0 ;\n\t"
        :
        : "r" ((uint64_t)Start), "i" (0)
        :
    );
}

#endif /*__ZSIM_HOOKS_H__*/
