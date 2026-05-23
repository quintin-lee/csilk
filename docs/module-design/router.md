# Router Design

csilk uses a **Radix Tree** (compact prefix tree) for efficient route matching. The router supports static routes, named parameters (`:param`), and wildcards (`*wildcard`).

## Node Structure

```mermaid
graph TB
    subgraph "csilk_router_node_t"
        SEG["segment: 'users'"]
        TYPE["type: STATIC | PARAM | WILDCARD"]
        MH["method_handlers: linked list\n  GET → [handler1, handler2, NULL]\n  POST → [handler1, NULL]"]
        CH["children[0..127]: fixed array\n  children_count: 3"]
    end
```

## Radix Tree Visualization

```mermaid
graph TB
    ROOT["/ (root, STATIC)"]

    ROOT --> API["api (STATIC)"]

    API --> V1["v1 (STATIC)"]
    API --> V2["v2 (STATIC)"]

    V1 --> US["users (STATIC)"]
    V2 --> US2["users (STATIC)"]

    US --> GET1["(handlers: GET)"]
    US2 --> GET2["(handlers: GET)"]

    ROOT --> USERS["users (STATIC)"]
    USERS --> PID[":id (PARAM)"]
    PID --> PROFILE["profile (STATIC)"]
    PID --> POSTS["posts (STATIC)"]

    PROFILE --> GET3["(handlers: GET)"]
    POSTS --> GET4["(handlers: GET)"]

    ROOT --> STATIC["static (STATIC)"]
    STATIC --> WF["*filepath (WILDCARD)"]
    WF --> GET5["(handlers: GET)"]
```

## Route Registration Flow

```mermaid
sequenceDiagram
    participant App
    participant Router as csilk_router_t
    participant Root as Root Node
    participant Node as Child Nodes

    App->>Router: csilk_router_add(r, "GET", "/api/v1/users", handlers, 1)

    Note over Router: Split path: ["api", "v1", "users"]

    loop For each segment
        Router->>Root: find_or_create("api", STATIC)
        Root->>Root: Search children for STATIC "api"
        alt Not found
            Root->>Node: node_new("api", STATIC)
            Root->>Root: children[idx] = new_node
        end
        Router->>Node: find_or_create("v1", STATIC)
        Node->>Node: Search children for STATIC "v1"
        alt Not found
            Node->>Node: node_new("v1", STATIC)
        end
        Router->>Node: find_or_create("users", STATIC)
    end

    Note over Node: At terminal node "users"

    Router->>Node: check_no_duplicate_method("GET")
    Node->>Node: malloc(method_handler)
    Node->>Node: mh->method = "GET"
    Node->>Node: mh->handlers = [handler, NULL]
    Node->>Node: prepend to handlers linked list
```

## Route Matching Flow

```mermaid
sequenceDiagram
    participant C as csilk_ctx_t
    participant R as match_node()
    participant N as Router Node
    participant P as "Path: /users/42/profile"

    C->>R: match_node(root, "GET", "/users/42/profile", ctx)

    R->>R: seg = get_next_segment() → "users"

    loop For each child of root
        R->>N: Child "users" (STATIC)
        N->>N: strcmp("users", "users") == 0 ✓
        R->>R: match_node(child, "GET", "/42/profile", ctx)
        Note over R: Recursive call with remaining path

        R->>R: seg = get_next_segment() → "42"
        loop For each child of "users"
            R->>N: Child ":id" (PARAM)
            N->>N: Type is PARAM → capture "42"
            N->>P: ctx->params[0] = {":id", "42"}
            R->>R: match_node(child, "GET", "/profile", ctx)

            R->>R: seg = get_next_segment() → "profile"
            loop For each child of ":id"
                R->>N: Child "profile" (STATIC)
                N->>N: strcmp("profile", "profile") == 0 ✓
                R->>R: match_node(child, "GET", "/", ctx)

                Note over R: Path exhausted (/ or empty)
                N->>N: Lookup method handler for "GET"
                N-->>R: return [handler, NULL]
            end
        end
    end

    R-->>C: ctx->handlers = matched_handler_array
    C->>C: ctx->handler_index = -1
```

## Parameter Matching

```mermaid
flowchart TB
    subgraph "Parameter Types"
        STATIC["STATIC: Exact string match\n/users/profile"]
        PARAM["PARAM: Named capture\n/users/:id\n  → ctx->params[0] = {':id', '42'}"]
        WILD["WILDCARD: Greedy suffix\n/static/*filepath\n  → ctx->params[0] = {'*filepath', 'css/app.css'}"]
    end
```

- **STATIC** nodes match exact path segments (case-sensitive).
- **PARAM** nodes match any single path segment and capture the value. Backtracking is supported if the match fails deeper in the tree.
- **WILDCARD** nodes greedily match the entire remaining path. The router stops traversal at the wildcard node and checks for a handler.

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Route Insertion | O(L) | L = path segment count |
| Route Matching (best) | O(S) | S = segment count, exact static match |
| Route Matching (worst) | O(N + P) | N = static children, P = param branches |
| Memory per Route | O(S) | One node per path segment |

The fixed-size children array (`CSILK_MAX_CHILDREN = 128`) trades some memory for CPU cache locality, ensuring fast linear scans over child nodes.
