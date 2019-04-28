#ifndef __TYPES_H__
#define __TYPES_H__

#include <cassert>
#include <limits>

// TODO: do this lit/clause size ifdef below better
// TODO: CLAUSE_64 doesn't work yet...

#define LIT_32 1
#define CLAUSE_32 1

#ifdef LIT_8
typedef int8_t lit_t;
#endif
#ifdef LIT_16
typedef int16_t lit_t;
#endif
#ifdef LIT_32
typedef int32_t lit_t;
#endif
#ifdef LIT_64
typedef int64_t lit_t;
#endif

#ifdef CLAUSE_8
typedef uint8_t clause_t;
#endif
#ifdef CLAUSE_16
typedef uint16_t clause_t;
#endif
#ifdef CLAUSE_32
typedef uint32_t clause_t;
#endif
#ifdef CLAUSE_64
typedef uint64_t clause_t;
#endif


constexpr lit_t lit_nil = lit_t(0);
constexpr clause_t clause_nil = std::numeric_limits<clause_t>::max();

enum ReturnValue {
    UNKNOWN = 0,
    SATISFIABLE = 10,
    UNSATISFIABLE = 20
};

#endif  // __TYPES_H__
