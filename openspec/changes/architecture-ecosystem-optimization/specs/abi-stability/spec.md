## ADDED Requirements

### Requirement: Opaque Structures
The `csilk_ctx_t` and `csilk_server_t` structs SHALL be opaque in the public headers to guarantee ABI stability.

#### Scenario: Compilation against public headers
- **WHEN** a user compiles an application against `include/csilk/csilk.h`
- **THEN** direct access to struct members (e.g., `c->request.method`) SHALL cause a compilation error due to incomplete types

### Requirement: Context Accessor API
The system SHALL provide getter and setter functions for all necessary context attributes (e.g., method, path, headers, body).

#### Scenario: Reading request method
- **WHEN** a handler calls `csilk_get_method(c)`
- **THEN** it SHALL return the HTTP method string safely
