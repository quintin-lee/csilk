#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/app/workflow.h"
#include "csilk/core/config/config.h"
#include "csilk/core/middleware.h"
#include "csilk/core/types.h"
#include "csilk/drivers/ai.h"
#include "csilk/drivers/db.h"
#include "csilk/drivers/vector.h"
#include "csilk/messaging/mq.h"
#include "../../src/workflow/workflow_internal.h"

typedef struct {
    const char* name;
    size_t      offset;
    size_t      size;
} abi_field_desc_t;

typedef struct {
    const char*             py_name;
    const char*             c_name;
    size_t                  size;
    const abi_field_desc_t* fields;
    size_t                  field_count;
} abi_struct_desc_t;

#define ABI_FIELD(m_type, m_field)                                                                 \
    {                                                                                              \
        .name = #m_field,                                                                          \
        .offset = offsetof(m_type, m_field),                                                       \
        .size = sizeof(((m_type){0}).m_field),                                                     \
    }

static const abi_field_desc_t f_circuit_breaker_config[] = {
    ABI_FIELD(csilk_circuit_breaker_config_t, failure_threshold),
    ABI_FIELD(csilk_circuit_breaker_config_t, recovery_timeout_ms),
};

static const abi_field_desc_t f_cors_config[] = {
    ABI_FIELD(csilk_cors_config_t, allow_origin),
    ABI_FIELD(csilk_cors_config_t, allow_methods),
    ABI_FIELD(csilk_cors_config_t, allow_headers),
    ABI_FIELD(csilk_cors_config_t, allow_credentials),
    ABI_FIELD(csilk_cors_config_t, max_age),
    ABI_FIELD(csilk_cors_config_t, allow_origin_list),
    ABI_FIELD(csilk_cors_config_t, echo_origin),
};

static const abi_field_desc_t f_data[] = {
    ABI_FIELD(csilk_data_t, type),
    ABI_FIELD(csilk_data_t, value),
    ABI_FIELD(csilk_data_t, free_fn),
    ABI_FIELD(csilk_data_t, meta),
};

static const abi_field_desc_t f_ai_meta[] = {
    ABI_FIELD(csilk_ai_meta_t, model),
    ABI_FIELD(csilk_ai_meta_t, prompt_tokens),
    ABI_FIELD(csilk_ai_meta_t, completion_tokens),
};

static const abi_field_desc_t f_ai_config[] = {
    ABI_FIELD(csilk_ai_config_t, model),
    ABI_FIELD(csilk_ai_config_t, system_msg),
    ABI_FIELD(csilk_ai_config_t, prompt),
    ABI_FIELD(csilk_ai_config_t, temperature),
    ABI_FIELD(csilk_ai_config_t, max_tokens),
    ABI_FIELD(csilk_ai_config_t, stream),
    ABI_FIELD(csilk_ai_config_t, max_history_messages),
};

static const abi_field_desc_t f_vector_search_config[] = {
    ABI_FIELD(csilk_vector_search_config_t, ai),
    ABI_FIELD(csilk_vector_search_config_t, embedding_model),
    ABI_FIELD(csilk_vector_search_config_t, db),
    ABI_FIELD(csilk_vector_search_config_t, collection),
    ABI_FIELD(csilk_vector_search_config_t, limit),
    ABI_FIELD(csilk_vector_search_config_t, input_template),
};

static const abi_field_desc_t f_wf_tool_entry[] = {
    ABI_FIELD(csilk_wf_tool_entry_t, name),
    ABI_FIELD(csilk_wf_tool_entry_t, description),
    ABI_FIELD(csilk_wf_tool_entry_t, parameters_json),
    ABI_FIELD(csilk_wf_tool_entry_t, fn),
    ABI_FIELD(csilk_wf_tool_entry_t, user_data),
};

static const abi_field_desc_t f_ai_tool_call[] = {
    ABI_FIELD(csilk_ai_tool_call_t, id),
    ABI_FIELD(csilk_ai_tool_call_t, name),
    ABI_FIELD(csilk_ai_tool_call_t, arguments),
};

static const abi_field_desc_t f_ai_message[] = {
    ABI_FIELD(csilk_ai_message_t, role),
    ABI_FIELD(csilk_ai_message_t, content),
    ABI_FIELD(csilk_ai_message_t, tool_calls),
    ABI_FIELD(csilk_ai_message_t, tool_call_count),
    ABI_FIELD(csilk_ai_message_t, tool_call_id),
};

static const abi_field_desc_t f_ai_chat_response[] = {
    ABI_FIELD(csilk_ai_chat_response_t, content),
    ABI_FIELD(csilk_ai_chat_response_t, reasoning_content),
    ABI_FIELD(csilk_ai_chat_response_t, tool_calls),
    ABI_FIELD(csilk_ai_chat_response_t, tool_call_count),
    ABI_FIELD(csilk_ai_chat_response_t, prompt_tokens),
    ABI_FIELD(csilk_ai_chat_response_t, completion_tokens),
    ABI_FIELD(csilk_ai_chat_response_t, total_tokens),
    ABI_FIELD(csilk_ai_chat_response_t, raw_response),
    ABI_FIELD(csilk_ai_chat_response_t, error_message),
};

static const abi_field_desc_t f_ai_embeddings_response[] = {
    ABI_FIELD(csilk_ai_embeddings_response_t, values),
    ABI_FIELD(csilk_ai_embeddings_response_t, dimension),
    ABI_FIELD(csilk_ai_embeddings_response_t, count),
    ABI_FIELD(csilk_ai_embeddings_response_t, prompt_tokens),
    ABI_FIELD(csilk_ai_embeddings_response_t, total_tokens),
    ABI_FIELD(csilk_ai_embeddings_response_t, error_message),
};

static const abi_field_desc_t f_ai_tool_function[] = {
    ABI_FIELD(csilk_ai_tool_function_t, name),
    ABI_FIELD(csilk_ai_tool_function_t, description),
    ABI_FIELD(csilk_ai_tool_function_t, parameters_json),
};

static const abi_field_desc_t f_ai_tool[] = {
    ABI_FIELD(csilk_ai_tool_t, type),
    ABI_FIELD(csilk_ai_tool_t, function),
};

static const abi_field_desc_t f_ai_chat_request[] = {
    ABI_FIELD(csilk_ai_chat_request_t, model),
    ABI_FIELD(csilk_ai_chat_request_t, messages),
    ABI_FIELD(csilk_ai_chat_request_t, message_count),
    ABI_FIELD(csilk_ai_chat_request_t, temperature),
    ABI_FIELD(csilk_ai_chat_request_t, top_p),
    ABI_FIELD(csilk_ai_chat_request_t, presence_penalty),
    ABI_FIELD(csilk_ai_chat_request_t, frequency_penalty),
    ABI_FIELD(csilk_ai_chat_request_t, max_tokens),
    ABI_FIELD(csilk_ai_chat_request_t, stop),
    ABI_FIELD(csilk_ai_chat_request_t, stop_count),
    ABI_FIELD(csilk_ai_chat_request_t, user),
    ABI_FIELD(csilk_ai_chat_request_t, stream),
    ABI_FIELD(csilk_ai_chat_request_t, on_chunk),
    ABI_FIELD(csilk_ai_chat_request_t, on_reasoning_chunk),
    ABI_FIELD(csilk_ai_chat_request_t, user_data),
    ABI_FIELD(csilk_ai_chat_request_t, timeout_ms),
    ABI_FIELD(csilk_ai_chat_request_t, tools),
    ABI_FIELD(csilk_ai_chat_request_t, tool_count),
    ABI_FIELD(csilk_ai_chat_request_t, tool_choice),
    ABI_FIELD(csilk_ai_chat_request_t, reasoning_effort),
    ABI_FIELD(csilk_ai_chat_request_t, has_enable_thinking),
    ABI_FIELD(csilk_ai_chat_request_t, enable_thinking),
    ABI_FIELD(csilk_ai_chat_request_t, chat_template_kwargs),
};

static const abi_field_desc_t f_ai_context[] = {
    ABI_FIELD(csilk_ai_context_t, messages),
    ABI_FIELD(csilk_ai_context_t, count),
    ABI_FIELD(csilk_ai_context_t, capacity),
    ABI_FIELD(csilk_ai_context_t, max_history),
};

static const abi_field_desc_t f_ai_stats[] = {
    ABI_FIELD(csilk_ai_stats_t, requests_total),
    ABI_FIELD(csilk_ai_stats_t, tokens_total),
    ABI_FIELD(csilk_ai_stats_t, prompt_tokens),
    ABI_FIELD(csilk_ai_stats_t, completion_tokens),
    ABI_FIELD(csilk_ai_stats_t, errors_total),
    ABI_FIELD(csilk_ai_stats_t, duration_us_total),
};

static const abi_field_desc_t f_db_stats[] = {
    ABI_FIELD(csilk_db_stats_t, queries_total),
    ABI_FIELD(csilk_db_stats_t, execs_total),
    ABI_FIELD(csilk_db_stats_t, errors_total),
    ABI_FIELD(csilk_db_stats_t, duration_us_total),
};

static const abi_field_desc_t f_multipart_part[] = {
    ABI_FIELD(csilk_multipart_part_t, name),
    ABI_FIELD(csilk_multipart_part_t, filename),
    ABI_FIELD(csilk_multipart_part_t, content_type),
    ABI_FIELD(csilk_multipart_part_t, data),
    ABI_FIELD(csilk_multipart_part_t, data_len),
    ABI_FIELD(csilk_multipart_part_t, ctx),
};

static const abi_field_desc_t f_mq_stats[] = {
    ABI_FIELD(csilk_mq_stats_t, published_total),
    ABI_FIELD(csilk_mq_stats_t, delivered_total),
    ABI_FIELD(csilk_mq_stats_t, failed_total),
    ABI_FIELD(csilk_mq_stats_t, queue_depth),
    ABI_FIELD(csilk_mq_stats_t, topic_count),
};

static const abi_field_desc_t f_vector_point[] = {
    ABI_FIELD(csilk_vector_point_t, id),
    ABI_FIELD(csilk_vector_point_t, vector),
    ABI_FIELD(csilk_vector_point_t, dimension),
    ABI_FIELD(csilk_vector_point_t, payload),
};

static const abi_field_desc_t f_vector_search_result[] = {
    ABI_FIELD(csilk_vector_search_result_t, id),
    ABI_FIELD(csilk_vector_search_result_t, score),
    ABI_FIELD(csilk_vector_search_result_t, payload),
};

static const abi_field_desc_t f_vector_search_response[] = {
    ABI_FIELD(csilk_vector_search_response_t, results),
    ABI_FIELD(csilk_vector_search_response_t, count),
    ABI_FIELD(csilk_vector_search_response_t, error_message),
};

static const abi_field_desc_t f_server_config[] = {
    ABI_FIELD(csilk_server_config_t, idle_timeout_ms),
    ABI_FIELD(csilk_server_config_t, read_timeout_ms),
    ABI_FIELD(csilk_server_config_t, write_timeout_ms),
    ABI_FIELD(csilk_server_config_t, request_timeout_ms),
    ABI_FIELD(csilk_server_config_t, max_body_size),
    ABI_FIELD(csilk_server_config_t, max_header_size),
    ABI_FIELD(csilk_server_config_t, max_url_size),
    ABI_FIELD(csilk_server_config_t, max_headers_count),
    ABI_FIELD(csilk_server_config_t, max_connections),
    ABI_FIELD(csilk_server_config_t, listen_backlog),
    ABI_FIELD(csilk_server_config_t, tcp_nodelay),
    ABI_FIELD(csilk_server_config_t, tcp_keepalive),
    ABI_FIELD(csilk_server_config_t, worker_threads),
    ABI_FIELD(csilk_server_config_t, enable_tls),
    ABI_FIELD(csilk_server_config_t, tls_cert_file),
    ABI_FIELD(csilk_server_config_t, tls_key_file),
    ABI_FIELD(csilk_server_config_t, tls_ca_file),
    ABI_FIELD(csilk_server_config_t, tls_verify_peer),
    ABI_FIELD(csilk_server_config_t, h2_push_enable),
    ABI_FIELD(csilk_server_config_t, h2_max_push_per_request),
    ABI_FIELD(csilk_server_config_t, enable_simd),
    ABI_FIELD(csilk_server_config_t, enable_arena_alignment),
    ABI_FIELD(csilk_server_config_t, enable_uring_sqpoll),
    ABI_FIELD(csilk_server_config_t, backpressure_max_queue_depth),
    ABI_FIELD(csilk_server_config_t, backpressure_max_latency_us),
    ABI_FIELD(csilk_server_config_t, enable_openapi),
};

#define ABI_STRUCT(m_py_name, m_c_type, m_field_arr)                                               \
    {                                                                                              \
        .py_name = #m_py_name,                                                                     \
        .c_name = #m_c_type,                                                                       \
        .size = sizeof(m_c_type),                                                                  \
        .fields = (m_field_arr),                                                                   \
        .field_count = sizeof(m_field_arr) / sizeof((m_field_arr)[0]),                             \
    }

static const abi_struct_desc_t g_abi_structs[] = {
    ABI_STRUCT(CsilkCircuitBreakerConfig, csilk_circuit_breaker_config_t, f_circuit_breaker_config),
    ABI_STRUCT(CsilkCorsConfig, csilk_cors_config_t, f_cors_config),
    ABI_STRUCT(CsilkData, csilk_data_t, f_data),
    ABI_STRUCT(CsilkAiMeta, csilk_ai_meta_t, f_ai_meta),
    ABI_STRUCT(CsilkAiConfig, csilk_ai_config_t, f_ai_config),
    ABI_STRUCT(CsilkVectorSearchConfig, csilk_vector_search_config_t, f_vector_search_config),
    ABI_STRUCT(CsilkWfToolEntry, csilk_wf_tool_entry_t, f_wf_tool_entry),
    ABI_STRUCT(CsilkAiToolCall, csilk_ai_tool_call_t, f_ai_tool_call),
    ABI_STRUCT(CsilkAiMessage, csilk_ai_message_t, f_ai_message),
    ABI_STRUCT(CsilkAiChatResponse, csilk_ai_chat_response_t, f_ai_chat_response),
    ABI_STRUCT(CsilkAiEmbeddingsResponse, csilk_ai_embeddings_response_t, f_ai_embeddings_response),
    ABI_STRUCT(CsilkAiToolFunction, csilk_ai_tool_function_t, f_ai_tool_function),
    ABI_STRUCT(CsilkAiTool, csilk_ai_tool_t, f_ai_tool),
    ABI_STRUCT(CsilkAiChatRequest, csilk_ai_chat_request_t, f_ai_chat_request),
    ABI_STRUCT(CsilkAiContext, csilk_ai_context_t, f_ai_context),
    ABI_STRUCT(CsilkAiStats, csilk_ai_stats_t, f_ai_stats),
    ABI_STRUCT(CsilkDbStats, csilk_db_stats_t, f_db_stats),
    ABI_STRUCT(CsilkMultipartPart, csilk_multipart_part_t, f_multipart_part),
    ABI_STRUCT(CsilkMqStats, csilk_mq_stats_t, f_mq_stats),
    ABI_STRUCT(CsilkVectorPoint, csilk_vector_point_t, f_vector_point),
    ABI_STRUCT(CsilkVectorSearchResult, csilk_vector_search_result_t, f_vector_search_result),
    ABI_STRUCT(CsilkVectorSearchResponse, csilk_vector_search_response_t, f_vector_search_response),
    ABI_STRUCT(CsilkServerConfig, csilk_server_config_t, f_server_config),
};

static void
dump_json(void)
{
    size_t n = sizeof(g_abi_structs) / sizeof(g_abi_structs[0]);
    printf("{\n");
    for (size_t i = 0; i < n; i++) {
        const abi_struct_desc_t* s = &g_abi_structs[i];
        printf("  \"%s\": {\n", s->py_name);
        printf("    \"c_name\": \"%s\",\n", s->c_name);
        printf("    \"size\": %zu,\n", s->size);
        printf("    \"fields\": [\n");
        for (size_t j = 0; j < s->field_count; j++) {
            const abi_field_desc_t* f = &s->fields[j];
            printf("      [\"%s\", %zu, %zu]%s\n",
                   f->name,
                   f->offset,
                   f->size,
                   (j + 1 < s->field_count) ? "," : "");
        }
        printf("    ]\n");
        printf("  }%s\n", (i + 1 < n) ? "," : "");
    }
    printf("}\n");
}

static void
verify_c_invariants(void)
{
    size_t n = sizeof(g_abi_structs) / sizeof(g_abi_structs[0]);
    assert(n == 23);
    for (size_t i = 0; i < n; i++) {
        const abi_struct_desc_t* s = &g_abi_structs[i];
        assert(s->size > 0);
        assert(s->field_count > 0);
        size_t prev_end = 0;
        for (size_t j = 0; j < s->field_count; j++) {
            const abi_field_desc_t* f = &s->fields[j];
            assert(f->size > 0);
            assert(f->offset >= prev_end);
            assert(f->offset + f->size <= s->size);
            prev_end = f->offset + f->size;
        }
    }
}

static int
resolve_repo_root(char* out, size_t out_sz)
{
    const char* file_path = __FILE__;
    const char* marker = "/tests/core/test_python_abi.c";
    const char* pos = strstr(file_path, marker);
    if (pos != nullptr) {
        size_t len = (size_t)(pos - file_path);
        if (len > 0 && len < out_sz) {
            memcpy(out, file_path, len);
            out[len] = '\0';
            return 0;
        }
    }
    return -1;
}

int
main(int argc, char** argv)
{
    if (argc >= 2 && strcmp(argv[1], "--dump-json") == 0) {
        dump_json();
        return 0;
    }

    verify_c_invariants();

    if (system("command -v python3 >/dev/null 2>&1") != 0) {
        printf("python3 not found; skipping Python ctypes ABI layout comparison\n");
        return 0;
    }

    char repo_root[PATH_MAX] = {0};
    if (resolve_repo_root(repo_root, sizeof(repo_root)) != 0) {
        fprintf(stderr, "Failed to resolve repository root from __FILE__ (%s)\n", __FILE__);
        return 1;
    }

    char script_path[PATH_MAX + 64] = {0};
    snprintf(script_path, sizeof(script_path), "%s/python/tests/test_abi_layout.py", repo_root);

    FILE* fp = fopen(script_path, "r");
    if (fp == nullptr) {
        fprintf(stderr, "ABI layout script not found at %s\n", script_path);
        return 1;
    }
    fclose(fp);

    char*       allocated_bin = realpath(argv[0], NULL);
    const char* probe_bin = (allocated_bin != nullptr) ? allocated_bin : argv[0];

    setenv("CSILK_ABI_PROBE_BIN", probe_bin, 1);
    setenv("CSILK_REQUIRE_ABI_PROBE", "1", 1);

    char cmd[PATH_MAX + 128] = {0};
    snprintf(cmd, sizeof(cmd), "python3 \"%s\"", script_path);
    int rc = system(cmd);
    free(allocated_bin);
    if (rc != 0) {
        fprintf(stderr, "Python ABI layout verification failed (rc=%d)\n", rc);
        return 1;
    }

    printf("All C/Python ABI layout checks passed!\n");
    return 0;
}
