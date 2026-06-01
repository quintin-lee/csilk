## ADDED Requirements

### Requirement: Cache-Line Aligned Arena
The Arena allocator SHALL ensure that every memory chunk and subsequent allocation is aligned to a 64-byte boundary.

#### Scenario: Allocating from Aligned Arena
- **WHEN** `csilk_arena_alloc` is called for any size
- **THEN** the returned pointer address SHALL be a multiple of 64

### Requirement: Configurable Arena Alignment
The system SHALL allow enabling or disabling strict 64-byte alignment via a compile-time or runtime configuration.

#### Scenario: Disabling strict alignment
- **WHEN** strict alignment is disabled in configuration
- **THEN** the Arena SHALL fallback to the default platform alignment (e.g., 8 or 16 bytes)
