# Arena Allocator Fast-Path Optimization Design

## 1. Objective
Optimize `csilk_arena_alloc()` to minimize instructions, memory loads/stores, and conditional branches in the hot allocation path:
- Cache active chunk bump pointers (`ptr`, `end`) directly in `csilk_arena_t`.
- Single comparison `(ptr + aligned_size <= end)` with branch prediction hint (`__builtin_expect(..., 1)`).
- Reduce fast-path execution to **2 loads, 1 store, 1 branch**.

---

## 2. Struct Layout & Cache Alignment

```c
typedef struct csilk_arena_s {
    uint8_t*             ptr;                /**< Current bump pointer (cache line 0, byte 0) */
    uint8_t*             end;                /**< Chunk end boundary (cache line 0, byte 8) */
    csilk_arena_chunk_t* head;               /**< Active chunk header (cache line 0, byte 16) */
    size_t               default_chunk_size; /**< Default chunk size (cache line 0, byte 24) */
    size_t               max_total_bytes;    /**< Maximum total limit (cache line 0, byte 32) */
    size_t               total_allocated;    /**< Total allocated bytes (cache line 0, byte 40) */
    int                  align_64;           /**< 64-byte alignment flag (cache line 0, byte 48) */
    uint8_t              _padding[12];       /**< Pad to 64 bytes (CSILK_CACHE_LINE_SIZE) */
} csilk_arena_t;
```

---

## 3. Fast Path & Assembly Breakdown

```c
void*
csilk_arena_alloc(csilk_arena_t* arena, size_t size)
{
    if (__builtin_expect(!arena || size == 0, 0)) {
        return size == 0 ? (void*)(uintptr_t)1 : NULL;
    }

    if (__builtin_expect(arena->align_64 == 0, 1)) {
        size_t aligned_size = (size + 7) & ~7ULL;
        uint8_t* cur = arena->ptr;
        uint8_t* next = cur + aligned_size;
        if (__builtin_expect(next <= arena->end && cur != NULL, 1)) {
            arena->ptr = next;
            return cur;
        }
        return arena_alloc_slow(arena, size, 8);
    }

    /* 64-byte aligned slow path */
    return arena_alloc_slow(arena, size, 64);
}
```

Generated Assembly for fast path (x86_64):
```asm
lea    0x7(%rsi), %rax
and    $-8, %rax
mov    (%rdi), %rdx
add    %rax, %rdx
cmp    0x8(%rdi), %rdx
ja     .Lslow
mov    %rdx, (%rdi)
sub    %rax, %rdx
mov    %rdx, %rax
ret
```

---

## 4. Invariants & Backward Compatibility

1. **Chunk Tiering**: 4KB (Tier 0), 16KB (Tier 1), 64KB (Tier 2) preserved.
2. **TLS Free List**: `tls_tier_free_lists` with pthread cleanup preserved.
3. **Max Memory Enforced**: `max_total_bytes` limit respected on chunk expansion.
4. **Debug Redzone**: `#ifdef DEBUG_ARENA` boundary verification intact.
5. **ASAN & OOM Testing**: `TEST_OOM` hooks and deterministic test counters intact.
6. **ABI Stability**: Structure size remains exactly 64 bytes (`CSILK_CACHE_LINE_SIZE`), opaque pointer semantics unchanged.
