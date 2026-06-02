## ADDED Requirements

### Requirement: Route Metadata Registration
The system SHALL provide a macro `CSILK_REGISTER_ROUTE_DOC` to register routes with OpenAPI metadata (tags, summary, responses).

#### Scenario: Registering a documented route
- **WHEN** a developer registers a route using `CSILK_REGISTER_ROUTE_DOC`
- **THEN** the metadata SHALL be stored internally during server initialization

### Requirement: OpenAPI JSON Endpoint
The system SHALL serve an auto-generated OpenAPI v3 JSON spec at `/openapi.json` if enabled in the configuration.

#### Scenario: Fetching the OpenAPI spec
- **WHEN** a client issues a GET request to `/openapi.json`
- **THEN** the server SHALL return a `200 OK` response with `application/json` Content-Type containing the valid OpenAPI v3 schema
