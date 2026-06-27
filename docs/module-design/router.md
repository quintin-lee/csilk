# Router Design

csilk uses a **Radix Tree** (compact prefix tree) for efficient route matching. The router supports static routes, named parameters (`:param`), and wildcards (`*wildcard`). Path matching is **SIMD-accelerated** via AVX2 (x86_64) and ARM NEON (aarch64) for byte-level prefix comparison — achieving ~50ns per lookup on AVX2 (P99 ≤ 100ns, 100K routes) and ~80ns per lookup on NEON. Routes **MUST** be registered before server start; the router is read-only during request processing. Wildcard routes **SHOULD** be registered last to ensure static/param routes take priority.

## Node Structure

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  },
  'flowchart': {'htmlLabels': true, 'curve': 'basis'}
}}%%
graph TB
    subgraph router_node["fa:fa-sitemap csilk_router_node_t"]
        SEG["fa:fa-tag segment: 'users'"]
        TYPE["fa:fa-list type: STATIC | PARAM | WILDCARD"]
        MH["fa:fa-link method_handlers: linked list\n  GET → [handler1, handler2, NULL]\n  POST → [handler1, NULL]"]
        CH["fa:fa-folder children[0..127]: fixed array\n  children_count: 3"]
    end
```

## Radix Tree Visualization

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  },
  'flowchart': {'htmlLabels': true, 'curve': 'basis'}
}}%%
graph TB
    ROOT["fa:fa-tree / (root, STATIC)"]

    ROOT --> API["fa:fa-folder api (STATIC)"]

    API --> V1["fa:fa-folder v1 (STATIC)"]
    API --> V2["fa:fa-folder v2 (STATIC)"]

    V1 --> US["fa:fa-file users (STATIC)"]
    V2 --> US2["fa:fa-file users (STATIC)"]

    US --> GET1["fa:fa-arrow-right (handlers: GET)"]
    US2 --> GET2["fa:fa-arrow-right (handlers: GET)"]

    ROOT --> USERS["fa:fa-folder users (STATIC)"]
    USERS --> PID["fa:fa-hashtag :id (PARAM)"]
    PID --> PROFILE["fa:fa-file profile (STATIC)"]
    PID --> POSTS["fa:fa-file posts (STATIC)"]

    PROFILE --> GET3["fa:fa-arrow-right (handlers: GET)"]
    POSTS --> GET4["fa:fa-arrow-right (handlers: GET)"]

    ROOT --> STATIC["fa:fa-folder static (STATIC)"]
    STATIC --> WF["fa:fa-asterisk *filepath (WILDCARD)"]
    WF --> GET5["fa:fa-arrow-right (handlers: GET)"]
```

## Route Registration Flow

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  }
}}%%
sequenceDiagram
    participant App as App
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
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  }
}}%%
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
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'background': '#2E3440',
    'primaryColor': '#81A1C1',
    'primaryBorderColor': '#4C566A',
    'primaryTextColor': '#ECEFF4',
    'secondaryColor': '#3B4252',
    'secondaryBorderColor': '#434C5E',
    'secondaryTextColor': '#D8DEE9',
    'lineColor': '#81A1C1',
    'textColor': '#ECEFF4',
    'mainBkg': '#3B4252',
    'nodeBorder': '#4C566A',
    'clusterBkg': '#2E3440',
    'clusterBorder': '#4C566A',
    'titleColor': '#ECEFF4',
    'edgeLabelBackground': '#3B4252',
    'nodeTextColor': '#ECEFF4'
  },
  'flowchart': {'htmlLabels': true, 'curve': 'basis'}
}}%%
flowchart TB
    subgraph param_types["fa:fa-tags Parameter Types"]
        STATIC["fa:fa-link STATIC: Exact string match<br/>/users/profile"]
        PARAM["fa:fa-hashtag PARAM: Named capture<br/>/users/:id<br/>  → ctx->params[0] = {':id', '42'}"]
        WILD["fa:fa-asterisk WILDCARD: Greedy suffix<br/>/static/*filepath<br/>  → ctx->params[0] = {'*filepath', 'css/app.css'}"]
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
