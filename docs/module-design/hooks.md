# Hook System Design

csilk provides a flexible Hook system that allows developers to listen to server and request lifecycle events. Unlike middleware, Hooks do not intercept the request flow but allow for side-effects like metrics gathering, custom logging, or resource cleanup.

## Hook Types

| Enum Value | Trigger Point | Callback Type |
|------------|---------------|---------------|
| `CSILK_HOOK_SERVER_START` | Just before `uv_run()` | `csilk_server_hook_handler_t` |
| `CSILK_HOOK_SERVER_STOP` | When `csilk_server_stop()` is called | `csilk_server_hook_handler_t` |
| `CSILK_HOOK_CONN_OPEN` | After TCP connection is accepted | `csilk_ctx_hook_handler_t` |
| `CSILK_HOOK_CONN_CLOSE` | When connection is fully closed | `csilk_ctx_hook_handler_t` |
| `CSILK_HOOK_REQUEST_BEGIN` | When HTTP request headers are parsed | `csilk_ctx_hook_handler_t` |
| `CSILK_HOOK_REQUEST_END` | After response is sent to client | `csilk_ctx_hook_handler_t` |

## Usage Examples

### 1. Connection Monitoring

```c
void on_connect(csilk_ctx_t* c) {
    CSILK_LOG_I("New connection established from IP: %s", csilk_get_client_ip(c));
}

void on_disconnect(csilk_ctx_t* c) {
    CSILK_LOG_I("Client disconnected");
}

int main() {
    // ... init server ...
    csilk_server_add_hook(server, CSILK_HOOK_CONN_OPEN, on_connect);
    csilk_server_add_hook(server, CSILK_HOOK_CONN_CLOSE, on_disconnect);
    // ... run server ...
}
```

### 2. Request Timing

```c
void on_req_end(csilk_ctx_t* c) {
    const char* req_id = csilk_get_request_id(c);
    int status = csilk_get_status(c);
    CSILK_LOG_I("Request %s finished with status %d", req_id, status);
}

int main() {
    // ...
    csilk_server_add_hook(server, CSILK_HOOK_REQUEST_END, on_req_end);
}
```

## Implementation Details

Hooks are stored in the `csilk_server_t` structure as an array of linked lists, one for each hook type. This allows multiple handlers to be registered for the same event.

Handlers are executed in the order they were registered (FIFO).

```c
typedef struct csilk_hook_node_s {
  void* handler;
  struct csilk_hook_node_s* next;
} csilk_hook_node_t;
```
