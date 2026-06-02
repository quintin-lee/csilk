## ADDED Requirements

### Requirement: Darwin Memory Allocation
The Arena allocator SHALL use `mach_vm_allocate` on macOS instead of standard `malloc` or `posix_memalign` to optimize page alignment and virtual memory management.

#### Scenario: Arena allocation on macOS
- **WHEN** the server starts and allocates its initial Arena chunk on an Apple platform (`__APPLE__`)
- **THEN** memory SHALL be allocated using `mach_vm_allocate`

### Requirement: Darwin Event Loop Tuning
The system SHALL configure the libuv event loop with optimal polling parameters for `kqueue` on macOS environments.

#### Scenario: Server start on macOS
- **WHEN** the server initializes the libuv loop on macOS
- **THEN** it SHALL apply Darwin-specific configuration flags to `uv_loop_init`
