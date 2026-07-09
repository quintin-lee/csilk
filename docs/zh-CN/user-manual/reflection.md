# Reflection 引擎使用指南

> **Version**: 0.3.0 | **Last updated**: 2026-06-29

csilk 的 Reflection（反射）引擎提供**编译期结构体内省**和**自动 JSON 绑定**。通过宏在编译时注册 C 结构体的字段布局，实现 struct ↔ JSON 的自动序列化/反序列化，无需手写 `cJSON_GetObjectItem` / `cJSON_Add*` 样板代码。

---

## 1. 核心概念

Reflection 引擎在**编译期**（通过 C11 `_Generic` 和 GCC `__attribute__((constructor))`）或**运行期**（通过 `csilk_reflect_register`）注册 C 结构体的字段描述信息，使框架能够：

- **Marshal（序列化）** — C struct → cJSON → JSON string
- **Unmarshal（反序列化）** — JSON string → cJSON → C struct
- **OpenAPI 集成** — 路由注册时自动生成请求/响应 Schema

---

## 2. 定义结构体并注册

### 2.1 宏注册（推荐，编译期自动注册）

```c
#include "csilk/reflection/reflect.h"

// 1. 定义结构体
typedef struct {
    int32_t    id;
    char*      name;
    char*      email;
    bool       is_active;
    double     score;
    int64_t    created_at;
} User;

// 2. 定义字段映射宏（每个字段一行）
#define USER_MAP(_) \
    _(User, id,         CSILK_TYPE_INT32,  sizeof(int32_t),  0, false, nullptr) \
    _(User, name,       CSILK_TYPE_STRING, sizeof(char*),    0, true,  nullptr) \
    _(User, email,      CSILK_TYPE_STRING, sizeof(char*),    0, true,  nullptr) \
    _(User, is_active,  CSILK_TYPE_BOOL,   sizeof(bool),     0, false, nullptr) \
    _(User, score,      CSILK_TYPE_DOUBLE, sizeof(double),   0, false, nullptr) \
    _(User, created_at, CSILK_TYPE_INT64,  sizeof(int64_t),  0, false, nullptr)

// 3. 自动注册（文件作用域，main() 之前执行）
CSILK_REGISTER_REFLECT(User, USER_MAP)
```

### 2.2 手动注册（运行期注册）

```c
void register_user_type(void) {
    csilk_field_desc_t fields[] = {
        {"id",         CSILK_TYPE_INT32,  offsetof(User, id),         sizeof(int32_t),  0, false, nullptr},
        {"name",       CSILK_TYPE_STRING, offsetof(User, name),       sizeof(char*),    0, true,  nullptr},
        {"email",      CSILK_TYPE_STRING, offsetof(User, email),      sizeof(char*),    0, true,  nullptr},
        {"is_active",  CSILK_TYPE_BOOL,   offsetof(User, is_active),  sizeof(bool),     0, false, nullptr},
        {"score",      CSILK_TYPE_DOUBLE, offsetof(User, score),      sizeof(double),   0, false, nullptr},
        {"created_at", CSILK_TYPE_INT64,  offsetof(User, created_at), sizeof(int64_t),  0, false, nullptr},
    };

    csilk_reflect_init();
    csilk_reflect_register("User", fields, 6);
}
```

---

## 3. 支持的数据类型

| 枚举值 | C 类型 | 说明 |
|:-------|:-------|:------|
| `CSILK_TYPE_INT8` | `int8_t` / `char` | 8 位有符号整数 |
| `CSILK_TYPE_UINT8` | `uint8_t` / `unsigned char` | 8 位无符号整数 |
| `CSILK_TYPE_INT16` | `int16_t` / `short` | 16 位有符号整数 |
| `CSILK_TYPE_UINT16` | `uint16_t` / `unsigned short` | 16 位无符号整数 |
| `CSILK_TYPE_INT32` | `int32_t` / `int` | 32 位有符号整数 |
| `CSILK_TYPE_UINT32` | `uint32_t` / `unsigned int` | 32 位无符号整数 |
| `CSILK_TYPE_INT64` | `int64_t` / `long long` | 64 位有符号整数 |
| `CSILK_TYPE_UINT64` | `uint64_t` / `unsigned long long` | 64 位无符号整数 |
| `CSILK_TYPE_FLOAT` | `float` | 单精度浮点数 |
| `CSILK_TYPE_DOUBLE` | `double` | 双精度浮点数 |
| `CSILK_TYPE_BOOL` | `_Bool` / `bool` | 布尔值 |
| `CSILK_TYPE_STRING` | `char*` 或 `char[N]` | 字符串（指针或定长数组） |
| `CSILK_TYPE_STRUCT` | 嵌套结构体 | 支持嵌套和指针 |

---

## 4. 序列化（Struct → JSON）

### 4.1 使用便利宏

```c
void user_to_json_example(void) {
    User u = {
        .id = 42,
        .name = "Alice",
        .email = "alice@example.com",
        .is_active = true,
        .score = 95.5,
        .created_at = 1719648000,
    };

    // csilk_marshal 自动推导类型名
    cJSON* json = csilk_marshal(&u);
    if (json) {
        char* str = cJSON_Print(json);
        printf("%s\n", str);
        // {"id":42,"name":"Alice","email":"alice@example.com",
        //  "is_active":true,"score":95.5,"created_at":1719648000}
        free(str);
        cJSON_Delete(json);
    }
}
```

### 4.2 使用显式 API

```c
cJSON* json = csilk_json_marshal("User", &u);
```

### 4.3 在 HTTP 处理器中使用

```c
void get_user_handler(csilk_ctx_t* c) {
    User u = {.id = 1, .name = "Alice", .email = "alice@example.com"};

    // 序列化为 cJSON 并直接响应
    cJSON* json = csilk_marshal(&u);
    if (json) {
        csilk_json(c, 200, json);
        cJSON_Delete(json);
    }
}
```

---

## 5. 反序列化（JSON → Struct）

### 5.1 使用便利宏

```c
void json_to_user_example(void) {
    const char* json_str = "{\"id\":42,\"name\":\"Bob\",\"email\":\"bob@example.com\"}";

    User u = {0};
    int ok = csilk_unmarshal(json_str, &u);

    if (ok) {
        printf("id=%d, name=%s, email=%s\n", u.id, u.name, u.email);
    }
}
```

### 5.2 使用显式 API

```c
int ok = csilk_json_unmarshal("User", json_str, &u);
```

### 5.3 从 HTTP 请求中绑定

```c
void create_user_handler(csilk_ctx_t* c) {
    // 获取请求体 JSON 字符串
    size_t body_len = 0;
    const char* body = csilk_get_body(c, &body_len);

    User u = {0};
    if (csilk_unmarshal(body, &u)) {
        // 自动解析了 id, name, email 等字段
        CSILK_LOG_I("Created user: %s (id=%d)", u.name, u.id);
        csilk_json_string(c, 201, "{\"status\":\"created\"}");
    } else {
        csilk_json_error(c, 400, "Invalid JSON body");
    }
}
```

---

## 6. 嵌套结构体

```c
typedef struct {
    double lat;
    double lng;
} Location;

typedef struct {
    int32_t    id;
    char*      name;
    Location   loc;        // 嵌套结构体（值语义）
    Location*  work_loc;   // 嵌套结构体（指针语义）
} Employee;

#define LOCATION_MAP(_) \
    _(Location, lat, CSILK_TYPE_DOUBLE, sizeof(double), 0, false, nullptr) \
    _(Location, lng, CSILK_TYPE_DOUBLE, sizeof(double), 0, false, nullptr)

#define EMPLOYEE_MAP(_) \
    _(Employee, id,       CSILK_TYPE_INT32,  sizeof(int32_t), 0, false, nullptr)    \
    _(Employee, name,     CSILK_TYPE_STRING, sizeof(char*),   0, true,  nullptr)    \
    _(Employee, loc,      CSILK_TYPE_STRUCT, sizeof(Location), 0, false, "Location") \
    _(Employee, work_loc, CSILK_TYPE_STRUCT, sizeof(Location*), 0, true, "Location")

CSILK_REGISTER_REFLECT(Location, LOCATION_MAP)
CSILK_REGISTER_REFLECT(Employee, EMPLOYEE_MAP)
```

用法：

```c
void nested_example(void) {
    Employee e = {
        .id = 1,
        .name = "Charlie",
        .loc = {.lat = 39.9042, .lng = 116.4074},
    };

    cJSON* json = csilk_marshal(&e);
    // {"id":1,"name":"Charlie","loc":{"lat":39.9042,"lng":116.4074}}

    // 反序列化也会递归处理嵌套
    const char* input = "{\"id\":2,\"name\":\"Dave\",\"loc\":{\"lat\":31.2304,\"lng\":121.4737}}";
    Employee e2 = {0};
    csilk_unmarshal(input, &e2);
}
```

---

## 7. 定长数组字段

```c
typedef struct {
    int32_t  id;
    double   coordinates[3];  // 定长数组
    char     tag[16];         // 定长字符串数组
} Point;

#define POINT_MAP(_) \
    _(Point, id,          CSILK_TYPE_INT32,  sizeof(int32_t), 0,      false, nullptr) \
    _(Point, coordinates, CSILK_TYPE_DOUBLE, sizeof(double),  3,      false, nullptr) \
    _(Point, tag,         CSILK_TYPE_STRING, sizeof(char),    16,     false, nullptr)

CSILK_REGISTER_REFLECT(Point, POINT_MAP)
```

```c
void array_example(void) {
    Point p = {.id = 1, .coordinates = {1.0, 2.0, 3.0}, .tag = "center"};
    cJSON* json = csilk_marshal(&p);
    // {"id":1,"coordinates":[1.0,2.0,3.0],"tag":"center"}
}
```

---

## 8. 与 OpenAPI 集成

注册反射类型后，路由注册时可以声明输入输出类型，自动生成 OpenAPI Schema：

```c
void setup_routes(csilk_app_t* app) {
    // POST /users — 入参 User，出参 User，自动生成 OpenAPI 文档
    csilk_app_add_route_extended(app, "POST", "/users",
        create_user_handler,
        "User",     // input_type — 请求体类型
        "User",     // output_type — 响应体类型
        "Create a new user",
        "Creates a user record from JSON body");
}
```

---

## 9. 查找已注册类型

```c
// 按名称查找类型
const csilk_reflect_entry_t* entry = csilk_reflect_lookup("User");
if (entry) {
    printf("Type '%s' has %zu fields\n", entry->name, entry->count);
    for (size_t i = 0; i < entry->count; i++) {
        printf("  Field: %s (offset=%zu)\n",
            entry->fields[i].json_key, entry->fields[i].offset);
    }
}
```

---

## 10. 完整示例

REST API 的 CRUD，使用 Reflection 自动序列化/反序列化：

```c
#include "csilk/csilk.h"
#include "csilk/reflection/reflect.h"

typedef struct {
    int32_t  id;
    char*    name;
    char*    email;
    bool     is_active;
} User;

#define USER_MAP(_) \
    _(User, id,        CSILK_TYPE_INT32,  sizeof(int32_t), 0, false, nullptr) \
    _(User, name,      CSILK_TYPE_STRING, sizeof(char*),   0, true,  nullptr) \
    _(User, email,     CSILK_TYPE_STRING, sizeof(char*),   0, true,  nullptr) \
    _(User, is_active, CSILK_TYPE_BOOL,   sizeof(bool),    0, false, nullptr)

CSILK_REGISTER_REFLECT(User, USER_MAP)

// 创建用户（自动反序列化请求体）
void create_user(csilk_ctx_t* c) {
    size_t len;
    const char* body = csilk_get_body(c, &len);

    User u = {0};
    if (!csilk_unmarshal(body, &u)) {
        csilk_json_error(c, 400, "Invalid request body");
        return;
    }

    // 业务逻辑：写入数据库...

    // 自动序列化响应
    cJSON* json = csilk_marshal(&u);
    csilk_json(c, 201, json);
    cJSON_Delete(json);
}

// 查询用户（自动序列化）
void get_user(csilk_ctx_t* c) {
    User u = {.id = 42, .name = "Alice", .email = "alice@example.com", .is_active = true};
    cJSON* json = csilk_marshal(&u);
    csilk_json(c, 200, json);
    cJSON_Delete(json);
}

int main() {
    csilk_app_t* app = csilk_app_new("config.yaml");

    // OpenAPI 集成：声明输入/输出类型
    csilk_app_add_route_extended(app, "POST", "/users", create_user,
        "User", "User", "Create user", "Creates a new user");
    csilk_app_add_route_extended(app, "GET", "/users/:id", get_user,
        nullptr, "User", "Get user", "Retrieves a user by ID");

    csilk_app_run(app, 8080);
    csilk_app_free(app);
    return 0;
}
```

---

## 11. 最佳实践

| 实践 | 说明 |
|:-----|:------|
| **SHOULD** 优先使用 `CSILK_REGISTER_REFLECT` 宏 | 编译期自动注册，无需显式调用初始化 |
| **SHOULD** 使用 `csilk_marshal` / `csilk_unmarshal` 便利宏 | 自动推导类型名，减少拼写错误 |
| **MAY** 配合 OpenAPI 集成 | `csilk_app_add_route_extended` 声明输入/输出类型 |
| **MUST** `CSILK_TYPE_STRUCT` 字段指定 `nested_type_name` | 反射引擎需通过名称查找嵌套类型定义 |
| **SHOULD** 字符串指针用 `true`（`is_pointer`） | 定长字符数组用 `false` + 设置数组长度 |
| **MUST NOT** 在字段映射中使用 `offsetof` 手动计算 | 宏 `CSILK_META_EXPAND` 自动使用 `offsetof` |

---

## 延伸阅读

| 文档 | 内容 |
|:-----|:------|
| [模块设计 — Reflection 引擎](../../docs/module-design/reflection.md) | Reflection 内部实现、类型注册、懒加载 |
| [配置指南 — OpenAPI](./configuration.md) | OpenAPI 文档生成相关配置 |
| [示例程序](../../examples/basic/example_app.c) | 含 Reflection 使用的完整应用示例 |
