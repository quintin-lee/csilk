# Formal Correctness Analysis — src/core/arena.c

> Date: 2026-05-30 | Method: Hoare-logic pre/post-condition and invariant verification

## 1. Chunk Structure Invariants

### Invariant I1 (chunk capacity)
```
∀ chunk ∈ arena:  chunk->used ≤ chunk->size   ∧   sizeof(csilk_arena_chunk_t) == CSILK_CACHE_LINE_SIZE
```

### Invariant I2 (chunk list)
```
arena->head == newest_chunk
arena->head->next → ... → nullptr  (singly-linked, no cycles)
```

### Invariant I3 (cache-line alignment)
```
∀ chunk:  (uintptr_t)chunk % CSILK_CACHE_LINE_SIZE == 0
∀ arena:  (uintptr_t)arena  % CSILK_CACHE_LINE_SIZE == 0
```

---

## 2. `csilk_arena_alloc()` — Core Allocation

### Pre-conditions (P)
```
P1: arena ≠ nullptr
P2: size ∈ [0, SIZE_MAX]
```

### Post-conditions (Q)
```
Q1: result == nullptr  ⇒  OOM or size overflow
Q2: result ≠ nullptr  ⇒  ∃ current_chunk = arena->head:
    result ∈ [chunk->data, chunk->data + chunk->size)
    ∧  chunk->used == old_used + aligned_size
    ∧  chunk->used ≤ chunk->size
```

### Proof

**Step A — Overflow guard (line 153)**
```
if (size > SIZE_MAX - 7) return nullptr;

Given:  8-byte alignment requires (size + 7) without overflow.
Proof:  For all size ≤ SIZE_MAX - 7:
          size + 7 ≤ SIZE_MAX   (no overflow)
        For size > SIZE_MAX - 7:
          return nullptr (safe rejection)
This guard is CORRECT and SUFFICIENT for line 157.
```

**Step B — Alignment (line 157)**
```
size = (size + 7) & ~7;
```
For any `size ∈ [0, SIZE_MAX - 7]`:
- `aligned = (size + 7) & ~7` is the smallest integer ≥ size that is a multiple of 8
- `aligned ≥ size` (correct — never truncates)
- `aligned % 8 == 0` (correct — always aligned)
- `aligned ≤ SIZE_MAX` (correct — no overflow after guard)

**Step C — Fast path: bump from head chunk (line 159–163)**
```
if (arena->head && (arena->head->size - arena->head->used) >= size)
```
By I1, `arena->head->used ≤ arena->head->size`, so subtraction is well-defined.
If enough space exists, this is a simple pointer increment — O(1), no system call.

**Step D — New chunk selection (line 165)**
```
chunk_size = size > arena->default_chunk_size ? size : arena->default_chunk_size;
```
**ISSUE FOUND**: If `size == 0 ∧ default_chunk_size == 0`, then `chunk_size == 0`.
A 0-size chunk has no usable data space; subsequent allocations into it will fail.
**Severity**: Low (callers always pass size > 0). **Mitigation**: Add guard.

**Step E — TLS cache reuse (line 171–174)**
```
if (chunk_size == CSILK_DEFAULT_ARENA_SIZE && tls_chunk_free_list)
```
Thread-local reuse of standard-size chunks. Only the head is popped — no iteration.
Thread-safe only when called from a single thread per arena (documented requirement).

**Step F — New chunk allocation (line 176)**
```
chunk = arena_aligned_alloc(sizeof(csilk_arena_chunk_t) + chunk_size);
```
**ISSUE FOUND**: `sizeof(csilk_arena_chunk_t) + chunk_size` can overflow.
Since `chunk_size ∈ [0, SIZE_MAX - 7]` and `sizeof(chunk) == CSILK_CACHE_LINE_SIZE == 64`:
```
max_chunk_size = SIZE_MAX - 7   (from Step A guard)
sizeof(chunk) + max_chunk_size = 64 + SIZE_MAX - 7 = SIZE_MAX + 57  → OVERFLOW
```
**Severity**: Extremely low (requires requesting ~18 EB allocation).
**Mitigation**: Add overflow guard before this addition.

**Step G — Chunk setup (lines 183–186)**
```
chunk->size = chunk_size;
chunk->used = size;         // already aligned in Step B
chunk->next = arena->head;  // prepend to list; preserves I2 (no cycles)
arena->head = chunk;
```
I1 (used ≤ size) holds because `size == chunk_size` when default_chunk_size ≤ size,
and `size ≤ chunk_size` (i.e., `default_chunk_size`) otherwise.
I2 preserved: head prepend, no cycles.

### Verification: OOM Safety

| Failure Point | Behavior | Consequence |
|:---|---:|:---|
| `arena_aligned_alloc(sizeof(arena))` in `_new` | Returns nullptr | Arena creation fails; `csilk_arena_new` returns nullptr |
| `arena_aligned_alloc(sizeof(chunk) + chunk_size)` in `_alloc` | Returns nullptr | `csilk_arena_alloc` returns nullptr; existing chunks preserved |
| `size > SIZE_MAX - 7` | Returns nullptr | Overflow prevented; no memory corruption |
| TEST_OOM counter | Returns nullptr | Simulates OOM; caller must handle gracefully |

**Conclusion**: All OOM paths return nullptr without corrupting state. **No use-after-free or double-free possible.**

---

## 3. `csilk_arena_reset()` — Zero-Allocation Reuse

### Pre-conditions
```
arena ≠ nullptr (null guarded)
```

### Post-conditions
```
∀ chunk ∈ arena:  chunk->used == 0
```

### Proof
```
while (curr) {
    curr->used = 0;
    curr = curr->next;
}
```
Simple linear walk. By I2, the list is finite and acyclic. Each chunk's `used` counter is zeroed.
No allocations, no frees — purely O(k) where k = chunk count.

---

## 4. `csilk_arena_free()` — Complete Cleanup

### Post-conditions
```
All chunks freed (returned to TLS cache or free())
arena header freed
arena pointer becomes dangling (caller must not reuse)
```

### TLS Cache Reuse Logic (lines 257–261)
```
if (curr->size == CSILK_DEFAULT_ARENA_SIZE && tls_chunk_count < MAX_TLS_ARENA_CHUNKS) {
    curr->next = tls_chunk_free_list;
    tls_chunk_free_list = curr;
    tls_chunk_count++;
} else {
    free(curr);
}
```
Standard-size chunks are recycled up to MAX_TLS_ARENA_CHUNKS (16).
Non-standard-size chunks are freed immediately.
The TLS list is **not** protected by a mutex — thread-confined by design
(each arena belongs to one thread/event-loop).

---

## 5. `arena_aligned_alloc()` — Cache-Line Aligned Allocation

### Alignment Proof
```
aligned_size = (size + CSILK_CACHE_LINE_SIZE - 1) & ~(CSILK_CACHE_LINE_SIZE - 1)
```

For `CSILK_CACHE_LINE_SIZE == 64` (power of 2):
- `(x + 63) & ~63` rounds x up to the nearest multiple of 64
- The mask `~63 == 0xFFFFFFFFFFFFFFC0` is correct for 64-bit size_t

**Potential overflow**: `size + CSILK_CACHE_LINE_SIZE - 1` has no guard.
If `size > SIZE_MAX - 63`, this wraps around to a small value, producing a
heap buffer that is too small for the requested size. The caller
(`csilk_arena_alloc`) already guards `size > SIZE_MAX - 7`, which is
stricter than the `63` needed here. So this is safe transitively.

**Platform compatibility**:
- C11+ (non-Apple): `aligned_alloc(64, aligned_size)` — CORRECT
- POSIX: `posix_memalign(&ptr, 64, aligned_size)` — CORRECT
- Fallback: `malloc(aligned_size)` — Loses alignment guarantee but is never used on modern platforms

---

## 6. Summary of Issues Found

| # | Issue | Severity | Status |
|---|-------|:--------:|:------:|
| 1 | `sizeof(chunk) + chunk_size` overflow | Extremely Low | Fix below |
| 2 | Zero-size chunk when `size == 0 ∧ default_chunk_size == 0` | Low | Fix below |
| 3 | `arena_aligned_alloc` overflow in round-up | None (protected transitively) | Documented |
| 4 | TLS free list not mutex-protected | None (design contract) | Documented |

---

## 7. Proposed Fixes

### Fix 1: Guard chunk_size allocation against overflow
```c
/* Guard against sizeof(chunk) + chunk_size overflow */
if (chunk_size > SIZE_MAX - sizeof(csilk_arena_chunk_t)) {
    return nullptr;
}
```
Add before line 176 in `csilk_arena_alloc`.

### Fix 2: Guard zero-size allocation
```c
if (size == 0) {
    // Return a non-null placeholder to avoid degenerate chunks.
    // Callers that request 0 bytes get a valid pointer that can
    // be written to but has no usable space — consistent with malloc(0).
    return (void*)(uintptr_t)1;
}
```
Add after line 155 in `csilk_arena_alloc`.

### Fix 3: Add chunk_size overflow guard for aligned_alloc
```c
if (chunk_size > SIZE_MAX - (CSILK_CACHE_LINE_SIZE - 1)) {
    return nullptr;
}
```
Add in `arena_aligned_alloc` before the round-up computation.

