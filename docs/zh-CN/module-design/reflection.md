# 反射引擎

反射引擎连接 C 结构体和 JSON，实现自动序列化和反序列化，而无需手动编写 JSON 解析代码。所有注册的类型元数据 **MUST** 在注册后不可变。字段查找 **MUST** 在 O(n) 线性扫描内完成（n = 注册字段，通常 ≤ 32）。封包一个平面结构体（≤ 8 个字段） **SHOULD** 在 ≤ 200ns 内完成。JSON 绑定 **MUST NOT** 在提供的竞技场分配器之外调用 `malloc`。

## 架构

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
flowchart TB
    subgraph registry["fa:fa-database 类型注册表"]
        REG["fa:fa-list csilk_reflect_entry_t<br/>  name: 'User'<br/>  fields: [<br/>    {'id', INT64, offset: 0}<br/>    {'name', STRING, offset: 8}<br/>    {'email', STRING, offset: 16}<br/>  ]"]
    end

    subgraph marshal["fa:fa-arrow-up 封包: C 结构体 -%gt; JSON"]
        M1["fa:fa-code csilk_json_marshal('User', &user)"]
        M2["fa:fa-search 在注册表中查找类型条目"]
        M3["fa:fa-repeat 按偏移量迭代字段"]
        M4["fa:fa-cube 构建 cJSON 对象"]
        M5["fa:fa-file-text cJSON_PrintUnformatted() -%gt; JSON 字符串"]
        M1 --> M2 --> M3 --> M4 --> M5
    end

    subgraph unmarshal["fa:fa-arrow-down 解包: JSON -%gt; C 结构体"]
        U1["fa:fa-code csilk_json_unmarshal('User', json_str, &user)"]
        U2["fa:fa-search cJSON_Parse(json_str)"]
        U3["fa:fa-search 在注册表中查找类型条目"]
        U4["fa:fa-repeat 按 json_key 迭代字段"]
        U5["fa:fa-edit 按偏移量 + 类型转换设置结构体字段"]
        U1 --> U2 --> U3 --> U4 --> U5
    end

    subgraph "Registration"
        INIT["csilk_reflect_init()\n(一次性初始化)"]
        INIT --> REGISTER["csilk_reflect_register('User', fields, count)"]
        REGISTER --> REG
        MACRO["CSILK_REGISTER_REFLECT(UserType, USER_FIELDS)\n(方便宏)"]
        MACRO --> REG
        AUTO["csilk_type_name(x)\n(C11 _Generic 推导)"]
        AUTO --> M1
        AUTO --> U1
    end
```

## 自动类型推导 (C11 _Generic)

Csilk 使用 C11 `_Generic` 在编译时自动确定类型名称字符串。这允许更简洁的 API：

```c
// 相反的写法:
csilk_json_marshal("User", &user);

// 您可以使用:
csilk_marshal(&user); // 宏推导出 "User"
```

### 支持的基本类型

以下类型由 `csilk_type_name` 和反射引擎原生支持：
- `bool`
- `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`
- `float`, `double`
- `string` (`char*` 或 `const char*`)

### 自定义类型映射

要启用用户定义结构体的自动推导，请在包含 `csilk.h` 之前扩展 `CSILK_USER_TYPE_MAP`：

```c
#undef CSILK_USER_TYPE_MAP
#define CSILK_USER_TYPE_MAP , struct User_s: "User"
#include "csilk/csilk.h"
```

---

## 顶层基本类型反射

引擎支持直接反射基本类型而无需包裹结构体：

```c
int count = 42;
char* json = csilk_marshal(&count); // 结果: "42"

bool active = true;
char* json_b = csilk_marshal(&active); // 结果: "true"

char* msg = "hello";
char* json_s = csilk_marshal(&msg); // 结果: "\"hello\""
```

---

## 数据流：JSON 绑定

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4','actorBorder':'#81A1C1','actorBkg':'#3B4252','actorTextColor':'#ECEFF4','signalColor':'#81A1C1','signalTextColor':'#D8DEE9','noteBkgColor':'#434C5E','noteTextColor':'#D8DEE9','loopTextColor':'#81A1C1','sequenceNumberColor':'#ECEFF4'}, 'sequence': {'actorFontSize': 14, 'noteFontSize': 12, 'messageFontSize': 12, 'mirrorActors': false}}}%%
sequenceDiagram
    participant Client
    participant Server
    participant CTX as fa:fa-cube csilk_ctx_t
    participant cJSON
    participant Reflect
    participant Struct as fa:fa-database User 结构体

    Client->>Server: POST /users (JSON body: {"id":1,"name":"Alice"})
    Server->>CTX: on_body() 累积 body
    Server->>CTX: on_message_complete()

    Handler->>CTX: csilk_bind_reflect(ctx, "User", &user)
    CTX->>cJSON: cJSON_Parse(request.body)
    cJSON-->>CTX: 解析后的 JSON 对象

    CTX->>Reflect: csilk_json_unmarshal("User", json_str, &user)
    Reflect->>Reflect: 在类型注册表中查找 "User"
    loop 对于类型条目中的每个字段
        Reflect->>cJSON: cJSON_GetObjectItem(json, "id")
        cJSON-->>Reflect: 整数值 1
        Reflect->>Struct: *(int64_t*)(ptr + offset) = 1

        Reflect->>cJSON: cJSON_GetObjectItem(json, "name")
        cJSON-->>Reflect: 字符串 "Alice"
        Reflect->>Struct: strcpy(ptr + offset, "Alice")
    end

    Reflect-->>Handler: 成功 (1)
```

---

## 数据流：通过反射的 JSON 响应

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4','actorBorder':'#81A1C1','actorBkg':'#3B4252','actorTextColor':'#ECEFF4','signalColor':'#81A1C1','signalTextColor':'#D8DEE9','noteBkgColor':'#434C5E','noteTextColor':'#D8DEE9','loopTextColor':'#81A1C1','sequenceNumberColor':'#ECEFF4'}, 'sequence': {'actorFontSize': 14, 'noteFontSize': 12, 'messageFontSize': 12, 'mirrorActors': false}}}%%
sequenceDiagram
    participant Handler
    participant CTX as fa:fa-cube csilk_ctx_t
    participant Reflect
    participant cJSON
    participant Struct as fa:fa-database User 结构体

    Note over Struct: fa:fa-info-circle user.id = 1, user.name = "Alice"

    Handler->>CTX: csilk_json_reflect(ctx, 200, "User", &user)
    CTX->>Reflect: csilk_json_marshal("User", &user)
    Reflect->>Reflect: 在类型注册表中查找 "User"

    Reflect->>cJSON: cJSON_CreateObject()
    loop 对于类型条目中的每个字段
        Reflect->>Struct: 读取 *(int64_t*)(ptr + offset) -> 1
        Reflect->>cJSON: cJSON_AddNumberToObject(json, "id", 1)
        Reflect->>Struct: 读取字符串在偏移量处
        Reflect->>cJSON: cJSON_AddStringToObject(json, "name", "Alice")
    end

    Reflect->>cJSON: cJSON_PrintUnformatted(json_obj)
    cJSON-->>Reflect: '{"id":1,"name":"Alice"}'

    Reflect-->>CTX: JSON 字符串 (托管)
    CTX->>CTX: response.body = json_str
    CTX->>CTX: response.body_is_managed = 1
    CTX->>CTX: _csilk_send_response()
```

---

## 支持的字段类型

| 字段类型 | C 类型 | JSON 类型 |
|----------|--------|-----------|
| `CSILK_TYPE_INT8` | `int8_t` | Number |
| `CSILK_TYPE_INT16` | `int16_t` | Number |
| `CSILK_TYPE_INT32` | `int32_t` | Number |
| `CSILK_TYPE_INT64` | `int64_t` | Number |
| `CSILK_TYPE_FLOAT` | `float` | Number |
| `CSILK_TYPE_DOUBLE` | `double` | Number |
| `CSILK_TYPE_BOOL` | `bool` | Boolean |
| `CSILK_TYPE_STRING` | `char[N]` 或 `char*` | String |
| `CSILK_TYPE_STRUCT` | 嵌套结构体 | Object |

---

## 注册示例

```c
#include "csilk/csilk.h"

// 定义结构体
typedef struct {
    int64_t id;
    char name[64];
    char email[128];
    bool active;
} User;

// 定义字段描述宏
#define USER_FIELDS(_) \
    _(User, id, CSILK_TYPE_INT64, 0, 0, false, NULL) \
    _(User, name, CSILK_TYPE_STRING, 64, 0, false, NULL) \
    _(User, email, CSILK_TYPE_STRING, 128, 0, false, NULL) \
    _(User, active, CSILK_TYPE_BOOL, 0, 0, false, NULL)

// 注册类型（在启动时一次性完成）
CSILK_REGISTER_REFLECT(User, USER_FIELDS);
```

---

## 初始化流程

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
flowchart TB
    S["fa:fa-play csilk_server_new()"] --> I["fa:fa-cog csilk_reflect_init()"]
    I --> CHECK{"fa:fa-question-circle 已初始化?"}
    CHECK -->|"否"| ALLOC["fa:fa-database 分配全局注册表"]
    ALLOC --> DONE["fa:fa-check-circle 准备就绪"]
    CHECK -->|"是"| DONE
    DONE --> REG["fa:fa-pencil-square-o CSILK_REGISTER_REFLECT 宏<br/>在全局作用域调用"]
    REG --> STORE["fa:fa-save 存储条目到注册表哈希图"]
```

---

## 深度释放和循环引用检测

为了支持复杂的嵌套结构图，反射引擎包括用于安全深度内存清理和验证以防止循环结构的机制。

### 深度结构体释放 (`csilk_struct_free_reflect`)

当将 JSON 解包到嵌套结构中时，子结构和字符串的指针可能是动态分配的。标准顺序 `free()` 调用会错过这些嵌套的动态结构，导致内存泄漏。

`csilk_struct_free_reflect` 使用其元数据递归遍历注册的结构体字段：
- 检查字段是否是另一个注册结构体的指针（`CSILK_TYPE_STRUCT`）。
- 检查字段是否是字符串指针（`CSILK_TYPE_STRING`）。
- 递归释放子结构，然后释放父结构指针。
- 采用**最大递归深度限制**（当前为 `32`），防止在极其深的嵌套布局上发生栈溢出。

### 循环引用检测

如果两个结构相互引用（直接或通过循环间接），递归编组、解组或深度释放将导致无限递归和栈溢出。

Csilk 通过在类型注册期间执行**静态循环引用检测**（使用深度优先搜索循环查找算法）来防止此问题：
- 当新类型通过 `CSILK_REGISTER_REFLECT` 注册时，注册表构建类型依赖有向图。
- 如果检测到循环（例如 `A -> B -> A`），会记录编译时或启动警告/错误。
- 在运行时，编组/解组等 API 尊重递归限制以优雅地失败，而不是崩溃进程。