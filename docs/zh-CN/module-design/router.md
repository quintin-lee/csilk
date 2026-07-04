# 路由器设计

csilk 使用 **基数树** (紧凑前缀树) 进行高效路由匹配。路由器支持静态路由、命名参数 (`:param`) 和通配符 (`*wildcard`)。路径匹配 **SIMD 加速** 通过 AVX2 (x86_64) 和 ARM NEON (aarch64) 进行字节级前缀比较 — 在 AVX2 上每个查找约 50ns (P99 ≤ 100ns, 100K 路由)，在 NEON 上约 80ns。路由 **MUST** 在服务器启动前注册；路由器在请求处理期间为只读。通配符路由 **SHOULD** 最后注册以确保静态/参数路由优先。

## 节点结构

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

## 基数树可视化

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

## 路由注册流程

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

## 路由匹配流程

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

## 参数匹配

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
    subgraph param_types["fa:fa-tags 参数类型"]
        STATIC["fa:fa-link STATIC: 精确字符串匹配<br/>/users/profile"]
        PARAM["fa:fa-hashtag PARAM: 命名捕获<br/>/users/:id<br/>  → ctx->params[0] = {':id', '42'}"]
        WILD["fa:fa-asterisk WILDCARD: 贪婪后缀<br/>/static/*filepath<br/>  → ctx->params[0] = {'*filepath', 'css/app.css'}"]
    end
```

- **STATIC** 节点精确匹配路径段（区分大小写）。
- **PARAM** 节点匹配任何单个路径段并捕获值。匹配失败时支持回溯。
- **WILDCARD** 节点贪婪匹配剩余整个路径。路由遍历在通配符节点停止，检查处理器是否存在。

## 性能特征

| 操作 | 复杂度 | 说明 |
|-----------|-----------|-------|
| 路由插入 | O(L) | L = 路径段数 |
| 路由匹配 (最好) | O(S) | S = 段数，精确静态匹配 |
| 路由匹配 (最差) | O(N + P) | N = 静态子节点数，P = 参数分支 |
| 每路由内存 | O(S) | 每个路径段一个节点 |

固定大小的子节点数组 (`CSILK_MAX_CHILDREN = 128`) 为 CPU 缓存局部性而 traded 一些内存，确保快速线性扫描子节点。