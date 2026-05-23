# Reflection Engine

The reflection engine bridges C structs and JSON, enabling automatic serialization and deserialization without manual JSON parsing code.

## Architecture

```mermaid
flowchart TB
    subgraph "Type Registry"
        REG["csilk_reflect_entry_t\n  name: 'User'\n  fields: [\n    {'id', INT64, offset: 0}\n    {'name', STRING, offset: 8}\n    {'email', STRING, offset: 16}\n  ]"]
    end

    subgraph "Marshal: C Struct → JSON"
        M1["csilk_json_marshal('User', &user)"]
        M2["Find type entry in registry"]
        M3["Iterate fields by offset"]
        M4["Build cJSON object"]
        M5["cJSON_PrintUnformatted() → JSON string"]
        M1 --> M2 --> M3 --> M4 --> M5
    end

    subgraph "Unmarshal: JSON → C Struct"
        U1["csilk_json_unmarshal('User', json_str, &user)"]
        U2["cJSON_Parse(json_str)"]
        U3["Find type entry in registry"]
        U4["Iterate fields by json_key"]
        U5["Set struct fields by offset + type conversion"]
        U1 --> U2 --> U3 --> U4 --> U5
    end

    subgraph "Registration"
        INIT["csilk_reflect_init()\n(one-time init)"]
        INIT --> REGISTER["csilk_reflect_register('User', fields, count)"]
        REGISTER --> REG
        MACRO["CSILK_REGISTER_REFLECT(UserType, USER_FIELDS)\n(convenience macro)"]
        MACRO --> REG
    end
```

## Data Flow: JSON Binding

```mermaid
sequenceDiagram
    participant Client
    participant Server
    participant CTX as csilk_ctx_t
    participant cJSON
    participant Reflect
    participant Struct as User struct

    Client->>Server: POST /users (JSON body: {"id":1,"name":"Alice"})
    Server->>CTX: on_body() accumulates body
    Server->>CTX: on_message_complete()

    Handler->>CTX: csilk_bind_reflect(ctx, "User", &user)
    CTX->>cJSON: cJSON_Parse(ctx->request.body)
    cJSON-->>CTX: parsed JSON object

    CTX->>Reflect: csilk_json_unmarshal("User", json_str, &user)
    Reflect->>Reflect: Find "User" in type registry
    loop For each field in type entry
        Reflect->>cJSON: cJSON_GetObjectItem(json, "id")
        cJSON-->>Reflect: integer value 1
        Reflect->>Struct: *(int64_t*)(ptr + offset) = 1

        Reflect->>cJSON: cJSON_GetObjectItem(json, "name")
        cJSON-->>Reflect: string "Alice"
        Reflect->>Struct: strcpy(ptr + offset, "Alice")
    end

    Reflect-->>Handler: success (1)
```

## Data Flow: JSON Response via Reflection

```mermaid
sequenceDiagram
    participant Handler
    participant CTX as csilk_ctx_t
    participant Reflect
    participant cJSON
    participant Struct as User struct

    Note over Struct: user.id = 1, user.name = "Alice"

    Handler->>CTX: csilk_json_reflect(ctx, 200, "User", &user)
    CTX->>Reflect: csilk_json_marshal("User", &user)
    Reflect->>Reflect: Find "User" in type registry

    Reflect->>cJSON: cJSON_CreateObject()
    loop For each field in type entry
        Reflect->>Struct: Read *(int64_t*)(ptr + offset) → 1
        Reflect->>cJSON: cJSON_AddNumberToObject(json, "id", 1)
        Reflect->>Struct: Read string at offset
        Reflect->>cJSON: cJSON_AddStringToObject(json, "name", "Alice")
    end

    Reflect->>cJSON: cJSON_PrintUnformatted(json_obj)
    cJSON-->>Reflect: '{"id":1,"name":"Alice"}'

    Reflect-->>CTX: JSON string (managed)
    CTX->>CTX: response.body = json_str
    CTX->>CTX: response.body_is_managed = 1
    CTX->>CTX: _csilk_send_response()
```

## Supported Field Types

| Field Type | C Type | JSON Type |
|-----------|--------|-----------|
| `CSILK_FIELD_INT8` | `int8_t` | Number |
| `CSILK_FIELD_INT16` | `int16_t` | Number |
| `CSILK_FIELD_INT32` | `int32_t` | Number |
| `CSILK_FIELD_INT64` | `int64_t` | Number |
| `CSILK_FIELD_FLOAT` | `float` | Number |
| `CSILK_FIELD_DOUBLE` | `double` | Number |
| `CSILK_FIELD_BOOL` | `int` (0/1) | Boolean |
| `CSILK_FIELD_STRING` | `char[N]` | String |
| `CSILK_FIELD_STRUCT` | nested struct | Object |
| `CSILK_FIELD_ARRAY` | array | Array |

## Registration Example

```c
// Define struct
typedef struct {
    int64_t id;
    char name[64];
    char email[128];
    int active;
} User;

// Define field descriptions macro
#define USER_FIELDS \
    CSILK_FIELD(id, CSILK_FIELD_INT64, 0) \
    CSILK_FIELD(name, CSILK_FIELD_STRING, sizeof(((User*)0)->name)) \
    CSILK_FIELD(email, CSILK_FIELD_STRING, sizeof(((User*)0)->email)) \
    CSILK_FIELD(active, CSILK_FIELD_BOOL, 0)

// Register type (done once at startup)
CSILK_REGISTER_REFLECT(User, USER_FIELDS);
```

## Init Flow

```mermaid
flowchart TB
    S["csilk_server_new()"] --> I["csilk_reflect_init()"]
    I --> CHECK["Already initialized?"]
    CHECK -->|No| ALLOC["Allocate global registry"]
    ALLOC --> DONE["Ready"]
    CHECK -->|Yes| DONE
    DONE --> REG["CSILK_REGISTER_REFLECT macros\nCalled at global scope"]
    REG --> STORE["Store entry in registry hash map"]
```
