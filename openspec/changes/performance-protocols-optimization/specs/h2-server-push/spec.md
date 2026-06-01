## ADDED Requirements

### Requirement: HTTP/2 Server Push API
The system SHALL provide an API `csilk_push_promise` to allow servers to preemptively send resources to the client over an HTTP/2 connection.

#### Scenario: Sending a Push Promise
- **WHEN** a handler calls `csilk_push_promise` for a resource (e.g., `/style.css`)
- **THEN** the system SHALL send a PUSH_PROMISE frame to the client over the existing HTTP/2 session

### Requirement: Push Promise Validation
The system SHALL only allow push promises on HTTP/2 or HTTP/3 connections where the client has not disabled server push.

#### Scenario: Attempting push on HTTP/1.1
- **WHEN** `csilk_push_promise` is called on an HTTP/1.1 connection
- **THEN** the system SHALL ignore the call or return an error status
