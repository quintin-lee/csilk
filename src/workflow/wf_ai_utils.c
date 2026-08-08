/**
 * @file wf_ai_utils.c
 * @brief Workflow AI memory helpers, JSON path traversal, context memory,
 *        and agent long-term memory store.
 *
 * @copyright MIT License
 */

#include "workflow_internal.h"
#include "csilk/core/sync.h"

/* --- Memory Helpers --- */

void*
csilk_wf_alloc(csilk_wf_ctx_t* ctx, size_t size)
{
    if (!ctx) {
        return nullptr;
    }
    csilk_mutex_lock(&ctx->arena_mutex);
    void* ptr = csilk_arena_alloc(ctx->arena, size);
    csilk_mutex_unlock(&ctx->arena_mutex);
    return ptr;
}

char*
csilk_wf_strdup(csilk_wf_ctx_t* ctx, const char* s)
{
    if (!s) {
        return nullptr;
    }
    size_t len = strlen(s);
    char*  news = csilk_wf_alloc(ctx, len + 1);
    if (news) {
        memcpy(news, s, len + 1);
    }
    return news;
}

csilk_data_t*
csilk_wf_data_new(csilk_wf_ctx_t* ctx, const char* type, void* value)
{
    csilk_data_t* data = csilk_wf_alloc(ctx, sizeof(csilk_data_t));
    if (data) {
        data->type = csilk_wf_strdup(ctx, type);
        data->value = value;
        data->free_fn = nullptr;
        data->meta = nullptr;
    }
    return data;
}

/** @brief Internal: traverse a cJSON tree following a dot-separated path. */
char*
_csilk_json_get_path(csilk_wf_ctx_t* ctx, cJSON* root, const char* path)
{
    if (!root || !path) {
        return nullptr;
    }

    cJSON* curr = root;
    char*  path_copy = strdup(path);
    char*  saveptr;
    char*  token = strtok_r(path_copy, ".", &saveptr);

    while (token && curr) {
        if (cJSON_IsArray(curr)) {
            curr = cJSON_GetArrayItem(curr, atoi(token));
        } else {
            curr = cJSON_GetObjectItemCaseSensitive(curr, token);
        }
        token = strtok_r(nullptr, ".", &saveptr);
    }

    char* result = nullptr;
    if (curr) {
        if (cJSON_IsString(curr)) {
            result = csilk_wf_strdup(ctx, curr->valuestring);
        } else if (cJSON_IsNumber(curr)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", curr->valuedouble);
            result = csilk_wf_strdup(ctx, buf);
        } else if (cJSON_IsBool(curr)) {
            result = csilk_wf_strdup(ctx, curr->valueint ? "true" : "false");
        } else if (cJSON_IsNull(curr)) {
            result = csilk_wf_strdup(ctx, "null");
        } else {
            char* tmp = cJSON_PrintUnformatted(curr);
            result = csilk_wf_strdup(ctx, tmp);
            free(tmp);
        }
    }

    free(path_copy);
    return result;
}

/* --- Short-Term Context Memory Store --- */

void
csilk_wf_ctx_set_memory(csilk_wf_ctx_t* ctx, const char* key, const char* value)
{
    if (!ctx || !key) {
        return;
    }
    csilk_mutex_lock(&ctx->mutex);
    csilk_wf_mem_node_t* curr = ctx->memory_head;
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            curr->value = value ? csilk_wf_strdup(ctx, value) : nullptr;
            csilk_mutex_unlock(&ctx->mutex);
            return;
        }
        curr = curr->next;
    }
    csilk_wf_mem_node_t* node = csilk_wf_alloc(ctx, sizeof(csilk_wf_mem_node_t));
    if (node) {
        node->key = csilk_wf_strdup(ctx, key);
        node->value = value ? csilk_wf_strdup(ctx, value) : nullptr;
        node->next = ctx->memory_head;
        ctx->memory_head = node;
    }
    csilk_mutex_unlock(&ctx->mutex);
}

const char*
csilk_wf_ctx_get_memory(csilk_wf_ctx_t* ctx, const char* key)
{
    if (!ctx || !key) {
        return nullptr;
    }
    csilk_mutex_lock(&ctx->mutex);
    csilk_wf_mem_node_t* curr = ctx->memory_head;
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            const char* val = curr->value;
            csilk_mutex_unlock(&ctx->mutex);
            return val;
        }
        curr = curr->next;
    }
    csilk_mutex_unlock(&ctx->mutex);
    return nullptr;
}

/* --- Agent Long-Term Memory Store --- */

struct csilk_agent_memory_s {
    csilk_ai_t*        ai;
    char*              embedding_model;
    csilk_vector_db_t* db;
    char*              collection;
};

csilk_agent_memory_t*
csilk_agent_memory_new(csilk_ai_t*        ai,
                       const char*        embedding_model,
                       csilk_vector_db_t* db,
                       const char*        collection)
{
    if (!ai || !db || !collection) {
        return nullptr;
    }
    csilk_agent_memory_t* mem = malloc(sizeof(csilk_agent_memory_t));
    if (!mem) {
        return nullptr;
    }
    mem->ai = ai;
    mem->embedding_model =
        embedding_model ? strdup(embedding_model) : strdup("text-embedding-ada-002");
    mem->db = db;
    mem->collection = strdup(collection);
    return mem;
}

int
csilk_agent_memory_store(csilk_agent_memory_t* mem,
                         const char*           id,
                         const char*           text,
                         const char*           metadata_json)
{
    if (!mem || !id || !text) {
        return -1;
    }
    csilk_ai_embeddings_response_t eres = {0};
    const char*                    texts[1] = {text};
    if (csilk_ai_embeddings(mem->ai, mem->embedding_model, texts, 1, &eres) != 0) {
        csilk_ai_embeddings_response_free(&eres);
        return -1;
    }
    if (eres.count == 0 || eres.dimension == 0) {
        csilk_ai_embeddings_response_free(&eres);
        return -1;
    }

    cJSON* payload = metadata_json ? cJSON_Parse(metadata_json) : cJSON_CreateObject();
    if (!payload) {
        payload = cJSON_CreateObject();
    }
    cJSON_AddStringToObject(payload, "text", text);

    csilk_vector_point_t pt = {
        .id = id, .vector = eres.values, .dimension = eres.dimension, .payload = payload};

    int rc = csilk_vector_db_upsert(mem->db, mem->collection, &pt, 1);
    cJSON_Delete(payload);
    csilk_ai_embeddings_response_free(&eres);
    return rc;
}

int
csilk_agent_memory_recall(csilk_agent_memory_t*           mem,
                          const char*                     query,
                          int                             limit,
                          csilk_vector_search_response_t* res)
{
    if (!mem || !query || !res) {
        return -1;
    }
    csilk_ai_embeddings_response_t eres = {0};
    const char*                    texts[1] = {query};
    if (csilk_ai_embeddings(mem->ai, mem->embedding_model, texts, 1, &eres) != 0) {
        csilk_ai_embeddings_response_free(&eres);
        return -1;
    }
    if (eres.count == 0 || eres.dimension == 0) {
        csilk_ai_embeddings_response_free(&eres);
        return -1;
    }

    int rc = csilk_vector_db_search(
        mem->db, mem->collection, eres.values, eres.dimension, limit > 0 ? limit : 5, res);
    csilk_ai_embeddings_response_free(&eres);
    return rc;
}

void
csilk_agent_memory_free(csilk_agent_memory_t* mem)
{
    if (!mem) {
        return;
    }
    free(mem->embedding_model);
    free(mem->collection);
    free(mem);
}
