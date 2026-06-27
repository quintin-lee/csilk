# AI Unified Interface

The AI module provides a provider-agnostic abstraction for integrating Large Language Models (LLMs) and other AI services into Csilk applications. All AI drivers **MUST** implement the `csilk_ai_driver_t` vtable interface. Chat completion **MUST NOT** block the event loop — use `csilk_ai_chat_async()` for non-blocking invocation. Streaming responses **SHOULD** be delivered via Server-Sent Events (SSE). Embedding dimension mismatch between model and index **MUST** be rejected at runtime.

## Architecture

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor':'#ECEFF4','secondaryColor':'#3B4252','secondaryBorderColor':'#434C5E','secondaryTextColor':'#D8DEE9','lineColor':'#81A1C1','textColor':'#ECEFF4','mainBkg':'#3B4252','nodeBorder':'#4C566A','clusterBkg':'#2E3440','clusterBorder':'#4C566A','titleColor':'#ECEFF4','edgeLabelBackground':'#3B4252','nodeTextColor':'#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
flowchart TB
    subgraph api["fa:fa-book Public API (csilk/drivers/ai.h)"]
        NEW["fa:fa-plus-circle csilk_ai_new(driver, key, base_url)"]
        CHAT["fa:fa-comment csilk_ai_chat(ai, req, res)"]
        ASYNC["fa:fa-bolt csilk_ai_chat_async(ai, req, cb)"]
        EMB["fa:fa-cube csilk_ai_embeddings(ai, model, input, ...)"]
    end

    subgraph engine["fa:fa-cogs Core Engine (ai.c)"]
        REG["fa:fa-list Driver Registry"]
        POOL["fa:fa-tasks libuv Thread Pool<br/>(for Async calls)"]
        RETRY["fa:fa-repeat Retry Mechanism<br/>(Exponential Backoff)"]
        CTX["fa:fa-folder-open Context Manager<br/>(History Windowing)"]
    end

    subgraph drivers["fa:fa-plug Drivers (src/drivers/)"]
        D1["fa:fa-cloud OpenAI Driver<br/>(libcurl + cJSON)"]
        D2["fa:fa-laptop Ollama Driver<br/>(Local AI)"]
    end

    NEW --> REG
    CHAT --> RETRY --> D1
    CHAT --> RETRY --> D2
    ASYNC --> POOL --> CHAT
    EMB --> D1
```

## Key Features

### 1. Unified Request/Response
Standardized structures for messages, chat completions, and embeddings ensure that your application logic remains decoupled from specific AI providers.

### 2. High-Performance Asynchronous I/O
Web applications should never be blocked by slow AI network calls. Csilk provides `csilk_ai_chat_async`, which leverages the framework's internal `libuv` thread pool to run AI requests in the background.

### 3. Native Streaming (SSE)
Support for Server-Sent Events (SSE) allows for "typewriter-style" real-time output.
- Set `stream = true` in the request.
- Provide an `on_chunk` callback to receive text deltas.

### 4. Function Calling (Tool Use)
Build autonomous agents by defining C function tools that the model can request to execute. Csilk automatically parses model-requested `tool_calls`.

### 6. AI Telemetry Integration

The AI engine integrates with the admin dashboard for real-time monitoring:

- **Model Call Tracking**: Every `csilk_ai_chat` and `csilk_ai_chat_async` call is recorded with model name, token counts, and latency.
- **Dashboard Integration**: AI metrics are exposed via the admin dashboard `/admin/stats` JSON endpoint as `ai_requests_total`, `ai_tokens_total`, and `ai_latency_ms`.
- **Error Tracking**: Failed AI requests are counted and categorized by error type (timeout, rate-limited, invalid response).

### 5. Automated Robustness
- **Automatic Retry**: Built-in exponential backoff handles transient network errors (502, 503) and Rate Limiting (429).
- **History Management**: `csilk_ai_context_t` manages a FIFO sliding window for conversation history to respect model token limits.

## Driver Configuration

### OpenAI Driver
- **Name**: `"openai"`
- **Dependencies**: `libcurl`
- **Supported APIs**: Chat Completion, Embeddings, Streaming, Function Calling.

### Ollama Driver
- **Name**: `"ollama"`
- **Purpose**: Local private LLM integration.
- **Default Base URL**: `http://localhost:11434`

## Usage Example (Async Chat)

```c
void on_chat_complete(int status, csilk_ai_chat_response_t* res, void* data) {
    if (status == 0) {
        printf("AI: %s\n", res->content);
    }
    csilk_ai_chat_response_free(res);
}

// ... inside a handler ...
csilk_ai_t* ai = csilk_ai_new("openai", key, NULL);
csilk_ai_chat_request_t req = { .model = "gpt-4", .messages = msgs, .message_count = 1 };

csilk_ai_chat_async(ai, &req, on_chat_complete, NULL);
```

## Usage Example (Function Calling)

```c
csilk_ai_tool_t tools[] = {
    { .type = "function", .function = { .name = "get_weather", .description = "..." } }
};

req.tools = tools;
req.tool_count = 1;

csilk_ai_chat(ai, &req, &res);

if (res.tool_call_count > 0) {
    // Model wants to call a function!
    execute_local_function(res.tool_calls[0].name, res.tool_calls[0].arguments);
}
```
