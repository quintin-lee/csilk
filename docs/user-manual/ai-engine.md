# AI 引擎使用指南

> **Version**: 0.5.0-dev | **Last updated**: 2026-06-29

csilk 的 AI 引擎提供了一个供应商无关的统一接口，用于集成大语言模型（LLM）和向量嵌入服务。支持 OpenAI、Ollama 等后端，可通过 `csilk_ai_register_driver` 注册自定义驱动。

**核心能力**：

- **聊天补全** — 同步/异步方式调用 LLM，支持 System/User/Assistant 消息
- **流式输出** — SSE 风格逐 token 推送，实现打字机效果
- **Embeddings** — 文本向量化，支持向量数据库集成
- **工具调用（Function Calling）** — 定义 C 函数工具供模型调用，构建自主 Agent
- **对话上下文** — 内置滑动窗口管理器，自动维护聊天历史

---

## 1. 快速开始

### 1.1 创建 AI 实例

```c
#include "csilk/drivers/ai.h"

int main() {
    // 创建 OpenAI 驱动实例
    csilk_ai_t* ai = csilk_ai_new("openai", "sk-your-api-key", nullptr);
    if (!ai) {
        CSILK_LOG_E("Failed to create AI instance");
        return 1;
    }

    // 使用完后释放
    csilk_ai_free(ai);
    return 0;
}
```

支持的驱动名称：

| 驱动 | 名称参数 | API Key | Base URL |
|:-----|:---------|:--------|:---------|
| OpenAI | `"openai"` | 必填 | 默认 `https://api.openai.com/v1` |
| Ollama | `"ollama"` | 传 `nullptr` | 默认 `http://localhost:11434` |

自定义 Base URL（适用于代理或兼容 API）：

```c
csilk_ai_t* ai = csilk_ai_new("openai", "sk-key",
    "https://your-proxy.example.com/v1");
```

### 1.2 基本聊天补全

```c
void basic_chat(void) {
    csilk_ai_t* ai = csilk_ai_new("openai", getenv("OPENAI_API_KEY"), nullptr);

    // 构造消息
    csilk_ai_message_t messages[] = {
        {.role = "system",    .content = "You are a helpful assistant."},
        {.role = "user",      .content = "What is csilk?"},
    };

    csilk_ai_chat_request_t req = {
        .model         = "gpt-4",
        .messages      = messages,
        .message_count = 2,
        .temperature   = 0.7,
        .max_tokens    = 500,
    };

    csilk_ai_chat_response_t res = {0};
    if (csilk_ai_chat(ai, &req, &res) == 0) {
        printf("AI: %s\n", res.content);
        printf("Tokens: %d prompt + %d completion\n",
               res.prompt_tokens, res.completion_tokens);
    } else {
        printf("Error: %s\n", res.error_message);
    }

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
}
```

### 1.3 在 HTTP 请求中使用

```c
void chat_handler(csilk_ctx_t* c) {
    // 解析用户输入
    cJSON* body = csilk_get_json(c);
    const char* user_msg = cJSON_GetObjectItem(body, "message")->valuestring;

    // 构造 AI 请求
    csilk_ai_message_t messages[] = {
        {.role = "system", .content = "You are a helpful assistant."},
        {.role = "user",   .content = user_msg},
    };

    csilk_ai_chat_request_t req = {
        .model         = "gpt-4",
        .messages      = messages,
        .message_count = 2,
        .temperature   = 0.7,
    };

    // 同步调用（会阻塞事件循环 — 生产环境请用异步版本）
    csilk_ai_t* ai = csilk_ai_new("openai", getenv("OPENAI_API_KEY"), nullptr);
    csilk_ai_chat_response_t res = {0};
    csilk_ai_chat(ai, &req, &res);

    // 返回结果
    csilk_json_string(c, 200, res.content ? res.content : "{}");

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
    cJSON_Delete(body);
}
```

> **注意**：同步调用 `csilk_ai_chat` 会阻塞当前线程。在 HTTP 处理器中**MUST** 使用异步版本 `csilk_ai_chat_async`。

---

## 2. 异步聊天补全

异步调用将 AI 请求 offload 到 libuv 线程池，完成后回调在主事件循环上执行，不会阻塞其他请求：

```c
typedef void (*csilk_ai_chat_async_cb)(int status,
    csilk_ai_chat_response_t* res, void* user_data);

void async_chat_handler(csilk_ctx_t* c) {
    cJSON* body = csilk_get_json(c);
    const char* user_msg = cJSON_GetObjectItem(body, "message")->valuestring;

    // 在 context 中存储 AI 实例和用户消息供回调使用
    csilk_ai_t* ai = csilk_ai_new("openai", getenv("OPENAI_API_KEY"), nullptr);
    csilk_set(c, "ai_instance", ai);

    // 构造请求
    csilk_ai_message_t* msgs = csilk_arena_alloc(
        csilk_get_arena(c), 2 * sizeof(csilk_ai_message_t));
    msgs[0] = (csilk_ai_message_t){.role = "system", .content = "You are helpful."};
    msgs[1] = (csilk_ai_message_t){.role = "user", .content = user_msg};

    csilk_ai_chat_request_t* req = csilk_arena_alloc(
        csilk_get_arena(c), sizeof(csil_ai_chat_request_t));
    *req = (csilk_ai_chat_request_t){
        .model         = "gpt-4",
        .messages      = msgs,
        .message_count = 2,
        .temperature   = 0.7,
    };

    // 异步调用 — 回调会在 AI 请求完成后执行
    csilk_ai_chat_async(ai, req, on_chat_done, c);

    cJSON_Delete(body);
}

// 异步回调
void on_chat_done(int status, csilk_ai_chat_response_t* res, void* user_data) {
    csilk_ctx_t* c = (csilk_ctx_t*)user_data;

    if (status == 0 && res->content) {
        csilk_string(c, 200, res->content);
    } else {
        csilk_json_error(c, 500, res->error_message ? res->error_message : "AI call failed");
    }

    csilk_ai_chat_response_free(res);
    csilk_ai_free(csil_get(c, "ai_instance"));
}
```

---

## 3. 流式输出（SSE）

设置 `stream = true` 并提供 `on_chunk` 回调，实现逐 token 推送：

```c
// 流式回调 — 每收到一个 token 就通过 SSE 推送给客户端
void on_chunk(const char* chunk, void* user_data) {
    csilk_ctx_t* c = (csilk_ctx_t*)user_data;
    csilk_sse_send(c, "token", chunk);
}

void stream_chat_handler(csilk_ctx_t* c) {
    // 初始化 SSE 连接
    csilk_sse_init(c);
    csilk_set_header(c, "Cache-Control", "no-cache");

    // 构造 AI 流式请求
    csilk_ai_message_t messages[] = {
        {.role = "user", .content = "Tell me a story about a brave knight."},
    };

    csilk_ai_chat_request_t req = {
        .model         = "gpt-4",
        .messages      = messages,
        .message_count = 1,
        .stream        = true,        // 启用流式
        .on_chunk      = on_chunk,    // token 回调
        .user_data     = c,           // 传入上下文供回调使用
    };

    csilk_ai_t* ai = csilk_ai_new("openai", getenv("OPENAI_API_KEY"), nullptr);
    csilk_ai_chat_response_t res = {0};
    csilk_ai_chat(ai, &req, &res);

    // 流结束，发送完成标记
    csilk_sse_send(c, "done", "stream complete");
    csilk_sse_close(c);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
}
```

---

## 4. 工具调用（Function Calling）

定义 C 函数工具，模型可自动选择调用：

### 4.1 定义工具

```c
// JSON Schema 定义工具参数
cJSON* build_weather_schema(void) {
    cJSON* schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");

    cJSON* properties = cJSON_AddObjectToObject(schema, "properties");

    cJSON* city = cJSON_CreateObject();
    cJSON_AddStringToObject(city, "type", "string");
    cJSON_AddStringToObject(city, "description", "City name, e.g. Beijing");
    cJSON_AddItemToObject(properties, "city", city);

    cJSON_AddStringToObject(schema, "required", "[\"city\"]");
    return schema;
}

// 工具函数（模型请求时由应用执行）
const char* get_weather(const char* city) {
    // 实际调用天气 API
    static char result[256];
    snprintf(result, sizeof(result),
        "{\"city\": \"%s\", \"temperature\": 22, \"condition\": \"sunny\"}", city);
    return result;
}
```

### 4.2 注册工具并调用

```c
void tool_call_handler(csilk_ctx_t* c) {
    csilk_ai_tool_function_t weather_fn = {
        .name        = "get_weather",
        .description = "Get current weather for a city",
        .parameters_json = build_weather_schema(),
    };

    csilk_ai_tool_t tool = {
        .type     = "function",
        .function = weather_fn,
    };

    csilk_ai_message_t messages[] = {
        {.role = "user", .content = "What's the weather in Beijing?"},
    };

    csilk_ai_chat_request_t req = {
        .model       = "gpt-4",
        .messages    = messages,
        .message_count = 1,
        .tools       = &tool,
        .tool_count  = 1,
        .tool_choice = "auto",
    };

    csilk_ai_t* ai = csilk_ai_new("openai", getenv("OPENAI_API_KEY"), nullptr);
    csilk_ai_chat_response_t res = {0};
    csilk_ai_chat(ai, &req, &res);

    // 检查模型是否请求了工具调用
    if (res.tool_call_count > 0) {
        for (size_t i = 0; i < res.tool_call_count; i++) {
            csilk_ai_tool_call_t* tc = &res.tool_calls[i];
            if (strcmp(tc->name, "get_weather") == 0) {
                const char* result = get_weather(tc->arguments);
                csilk_string(c, 200, result);
            }
        }
    } else if (res.content) {
        csilk_string(c, 200, res.content);
    }

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
    cJSON_Delete((cJSON*)weather_fn.parameters_json);
}
```

---

## 5. 对话上下文管理

`csilk_ai_context_t` 提供滑动窗口消息历史管理，自动裁剪超出限制的旧消息：

```c
void conversation_handler(csilk_ctx_t* c) {
    // 创建对话上下文，最多保留 20 条消息
    csilk_ai_context_t* ctx = csilk_ai_context_new(20);

    // 添加系统消息
    csilk_ai_context_add(ctx, "system",
        "You are a travel assistant. Be concise.");

    // 模拟多轮对话
    const char* user_questions[] = {
        "Recommend a good hotel in Tokyo.",
        "What's the best time to visit?",
        "How much does a flight cost?",
    };

    for (size_t i = 0; i < 3; i++) {
        csilk_ai_context_add(ctx, "user", user_questions[i]);

        csilk_ai_chat_request_t req = {
            .model         = "gpt-4",
            .messages      = ctx->messages,
            .message_count = ctx->count,
        };

        csilk_ai_t* ai = csilk_ai_new("openai", getenv("OPENAI_API_KEY"), nullptr);
        csilk_ai_chat_response_t res = {0};
        csilk_ai_chat(ai, &req, &res);

        if (res.content) {
            csilk_ai_context_add(ctx, "assistant", res.content);
            printf("AI: %s\n", res.content);
        }

        csilk_ai_chat_response_free(&res);
        csilk_ai_free(ai);
    }

    csilk_ai_context_free(ctx);
}
```

---

## 6. Embeddings（文本向量化）

将文本转为向量，用于语义搜索和 RAG：

```c
void embeddings_example(void) {
    csilk_ai_t* ai = csilk_ai_new("openai", getenv("OPENAI_API_KEY"), nullptr);

    const char* inputs[] = {
        "csilk is a high-performance HTTP framework",
        "It supports AI workflows and vector databases",
    };

    csilk_ai_embeddings_response_t res = {0};
    int rc = csilk_ai_embeddings(ai, "text-embedding-3-small",
                                  inputs, 2, &res);

    if (rc == 0) {
        printf("Dimensions: %zu, Count: %zu\n", res.dimension, res.count);
        printf("First vector first 5 values: %.4f, %.4f, %.4f, %.4f, %.4f\n",
               res.values[0], res.values[1], res.values[2],
               res.values[3], res.values[4]);

        // 与向量数据库（Qdrant/Milvus）集成
        // vector_db_insert(collection_id, inputs[0], &res.values[0], res.dimension);
    }

    csilk_ai_embeddings_response_free(&res);
    csilk_ai_free(ai);
}
```

---

## 7. 统计与监控

```c
void ai_stats_handler(csilk_ctx_t* c) {
    csilk_ai_stats_t stats;
    csilk_ai_get_stats(&stats);

    char* json = csilk_ai_stats_to_json(&stats);
    csilk_string(c, 200, json);
    free(json);
}
```

统计字段：

| 字段 | 类型 | 说明 |
|:-----|:----:|:-----|
| `requests_total` | uint64 | 总请求数 |
| `tokens_total` | uint64 | 总 token 数 |
| `prompt_tokens` | uint64 | Prompt token 数 |
| `completion_tokens` | uint64 | 生成 token 数 |
| `errors_total` | uint64 | 失败请求数 |
| `duration_us_total` | uint64 | 总耗时（微秒） |

---

## 8. 注册自定义驱动

对接私有 AI 服务或第三方 API：

```c
// 实现驱动接口
void* my_init(const char* api_key, const char* base_url) {
    MyState* state = calloc(1, sizeof(MyState));
    state->base_url = strdup(base_url ? base_url : "https://my-ai.example.com");
    return state;
}

int my_chat(void* state, const csilk_ai_chat_request_t* req,
             csilk_ai_chat_response_t* res) {
    // 实现 HTTP 调用...
    return 0;
}

int my_embeddings(void* state, const char* model,
                   const char** input, size_t count,
                   csilk_ai_embeddings_response_t* res) {
    // 实现 Embeddings 调用...
    return 0;
}

void my_free(void* state) {
    free(state);
}

static csilk_ai_driver_t my_driver = {
    .name       = "my-provider",
    .init       = my_init,
    .chat       = my_chat,
    .embeddings = my_embeddings,
    .free       = my_free,
};

void register_my_driver(void) {
    csilk_ai_register_driver(&my_driver);
}

// 使用自定义驱动
csilk_ai_t* ai = csilk_ai_new("my-provider", nullptr, "https://my-ai.example.com");
```

---

## 9. 完整示例

结合 HTTP、异步 AI 调用和 SSE 流式输出的聊天 API：

```c
#include "csilk/csilk.h"
#include "csilk/drivers/ai.h"

void on_stream_chunk(const char* chunk, void* user_data) {
    csilk_ctx_t* c = (csilk_ctx_t*)user_data;
    csilk_sse_send(c, "token", chunk);
}

void chat_stream_endpoint(csilk_ctx_t* c) {
    cJSON* body = csilk_get_json(c);
    const char* msg = cJSON_GetObjectItem(body, "message")->valuestring;

    csilk_sse_init(c);

    csilk_ai_message_t messages[] = {
        {.role = "system", .content = "You are a helpful coding assistant."},
        {.role = "user",   .content = msg},
    };

    csilk_ai_chat_request_t req = {
        .model         = "gpt-4",
        .messages      = messages,
        .message_count = 2,
        .stream        = true,
        .on_chunk      = on_stream_chunk,
        .user_data     = c,
    };

    csilk_ai_t* ai = csilk_ai_new("openai", getenv("OPENAI_API_KEY"), nullptr);
    csilk_ai_chat_response_t res = {0};
    csilk_ai_chat(ai, &req, &res);

    csilk_sse_send(c, "done", "ok");
    csilk_sse_close(c);

    csilk_ai_chat_response_free(&res);
    csilk_ai_free(ai);
    cJSON_Delete(body);
}

int main() {
    csilk_app_t* app = csilk_app_new("config.yaml");
    csilk_app_post(app, "/chat/stream", chat_stream_endpoint);
    csilk_app_run(app, 8080);
    csilk_app_free(app);
    return 0;
}
```

---

## 10. 最佳实践

| 实践 | 说明 |
|:-----|:------|
| **MUST** 在 HTTP 处理器中使用异步 API | `csilk_ai_chat_async` 防止阻塞事件循环 |
| **SHOULD** API Key 通过环境变量注入 | 不要硬编码在源码中，使用 `getenv("OPENAI_API_KEY")` |
| **SHOULD** 设置 `max_tokens` | 防止模型无限生成导致超时 |
| **SHOULD** 流式输出配合 SSE | `csilk_sse_init/send/close` 实现打字机效果 |
| **MAY** 使用 Context Manager | `csilk_ai_context_t` 自动管理多轮对话历史 |
| **MUST** 释放所有响应 | `csilk_ai_chat_response_free` / `csilk_ai_embeddings_response_free` |
| **MAY** 注册自定义驱动 | 通过 `csilk_ai_register_driver` 对接任意 AI 服务 |

---

## 延伸阅读

| 文档 | 内容 |
|:-----|:------|
| [模块设计 — AI 引擎](../../docs/module-design/ai.md) | AI 内部架构、驱动注册、重试机制 |
| [高级用法 — AI 工作流](./advanced-usage.md#ai-workflow-orchestration-ai-工作流编排) | AI 工作流编排与 Python 集成 |
| [Workflow 引擎设计](../../docs/module-design/workflow.md) | DAG 工作流引擎 |
| [配置指南](./configuration.md) | AI 供应商 Base URL 等配置选项 |
