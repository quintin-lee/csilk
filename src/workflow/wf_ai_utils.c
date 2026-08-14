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

/**
 * @brief Allocates memory from the workflow execution context's arena.
 *
 * Thread-safe allocation backed by the context arena (guarded by
 * ctx->arena_mutex). Allocations live until the context is destroyed.
 *
 * @param ctx  The workflow execution context (must not be NULL).
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on NULL ctx / failure.
 */
void*
csilk_wf_alloc(csilk_wf_ctx_t* ctx, size_t size)
{
    if (!ctx) {
        return NULL;
    }
    csilk_mutex_lock(&ctx->arena_mutex);
    void* ptr = csilk_arena_alloc(ctx->arena, size);
    csilk_mutex_unlock(&ctx->arena_mutex);
    return ptr;
}

/**
 * @brief Duplicates a string into the context arena.
 *
 * @param ctx The workflow execution context (must not be NULL).
 * @param s   Null-terminated source string (may be NULL, returns NULL).
 * @return Arena-allocated copy of s, or NULL on NULL input / allocation failure.
 */
char*
csilk_wf_strdup(csilk_wf_ctx_t* ctx, const char* s)
{
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char*  news = csilk_wf_alloc(ctx, len + 1);
    if (news) {
        memcpy(news, s, len + 1);
    }
    return news;
}

/**
 * @brief Creates a csilk_data_t wrapper in the context arena.
 *
 * The returned data carries a strdup'd type string and the supplied value
 * pointer; free_fn and meta are initialized to NULL.
 *
 * @param ctx   The workflow execution context (must not be NULL).
 * @param type  MIME/content type string (will be copied; must not be NULL).
 * @param value Opaque payload pointer (may be NULL; ownership passes to caller
 *              to free via free_fn).
 * @return New csilk_data_t, or NULL on allocation failure.
 */
csilk_data_t*
csilk_wf_data_new(csilk_wf_ctx_t* ctx, const char* type, void* value)
{
    csilk_data_t* data = csilk_wf_alloc(ctx, sizeof(csilk_data_t));
    if (data) {
        data->type = csilk_wf_strdup(ctx, type);
        data->value = value;
        data->free_fn = NULL;
        data->meta = NULL;
    }
    return data;
}

/** @brief Internal: traverse a cJSON tree following a dot-separated path. */
char*
_csilk_json_get_path(csilk_wf_ctx_t* ctx, csilk_json_t* root, const char* path)
{
    if (!root || !path) {
        return NULL;
    }

    csilk_json_t* curr = root;
    char*         path_copy = strdup(path);
    char*         saveptr;
    char*         token = strtok_r(path_copy, ".", &saveptr);

    while (token && curr) {
        if (csilk_json_is_array(curr)) {
            curr = csilk_json_array_get(curr, atoi(token));
        } else {
            curr = csilk_json_get(curr, token);
        }
        token = strtok_r(NULL, ".", &saveptr);
    }

    char* result = NULL;
    if (curr) {
        if (csilk_json_is_string(curr)) {
            result = csilk_wf_strdup(ctx, csilk_json_string_value(curr));
        } else if (csilk_json_is_number(curr)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", csilk_json_number_value(curr));
            result = csilk_wf_strdup(ctx, buf);
        } else if (csilk_json_is_bool(curr)) {
            result = csilk_wf_strdup(ctx, csilk_json_bool_value(curr) ? "true" : "false");
        } else if (csilk_json_is_null(curr)) {
            result = csilk_wf_strdup(ctx, "null");
        } else {
            char* tmp = csilk_json_serialize(curr, NULL);
            result = csilk_wf_strdup(ctx, tmp);
            free(tmp);
        }
    }

    free(path_copy);
    return result;
}

/* --- Short-Term Context Memory Store --- */

/**
 * @brief Stores a key/value string in the workflow execution context memory.
 *
 * Updates the value if the key already exists, otherwise prepends a new entry.
 * The value (and key) are copied into the context arena. Guarded by ctx->mutex.
 *
 * @param ctx   The workflow execution context (must not be NULL).
 * @param key   Memory key (must not be NULL).
 * @param value Value string (may be NULL to clear).
 * @note No-op if ctx or key is NULL.
 */
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
            curr->value = value ? csilk_wf_strdup(ctx, value) : NULL;
            csilk_mutex_unlock(&ctx->mutex);
            return;
        }
        curr = curr->next;
    }
    csilk_wf_mem_node_t* node = csilk_wf_alloc(ctx, sizeof(csilk_wf_mem_node_t));
    if (node) {
        node->key = csilk_wf_strdup(ctx, key);
        node->value = value ? csilk_wf_strdup(ctx, value) : NULL;
        node->next = ctx->memory_head;
        ctx->memory_head = node;
    }
    csilk_mutex_unlock(&ctx->mutex);
}

/**
 * @brief Retrieves a value from the workflow execution context memory.
 *
 * @param ctx The workflow execution context (must not be NULL).
 * @param key Memory key to look up (must not be NULL).
 * @return The stored value string, or NULL if absent / invalid args.
 * @note The returned pointer is owned by the context arena and remains valid
 *       for the lifetime of the context. Guarded by ctx->mutex.
 */
const char*
csilk_wf_ctx_get_memory(csilk_wf_ctx_t* ctx, const char* key)
{
    if (!ctx || !key) {
        return NULL;
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
    return NULL;
}

/* --- Agent Long-Term Memory Store --- */

struct csilk_agent_memory_s {
    csilk_ai_t*        ai;
    char*              embedding_model;
    csilk_vector_db_t* db;
    char*              collection;
};

/**
 * @brief Creates a long-term agent memory store backed by a vector database.
 *
 * @param ai              AI client used for embeddings (must not be NULL).
 * @param embedding_model Embedding model name (defaults if NULL).
 * @param db              Vector database handle (must not be NULL).
 * @param collection      Target collection name (must not be NULL).
 * @return New csilk_agent_memory_t, or NULL on invalid args / allocation failure.
 */
csilk_agent_memory_t*
csilk_agent_memory_new(csilk_ai_t*        ai,
                       const char*        embedding_model,
                       csilk_vector_db_t* db,
                       const char*        collection)
{
    if (!ai || !db || !collection) {
        return NULL;
    }
    csilk_agent_memory_t* mem = malloc(sizeof(csilk_agent_memory_t));
    if (!mem) {
        return NULL;
    }
    mem->ai = ai;
    mem->embedding_model =
        embedding_model ? strdup(embedding_model) : strdup("text-embedding-ada-002");
    mem->db = db;
    mem->collection = strdup(collection);
    return mem;
}

/**
 * @brief Stores a text memory entry (with optional metadata) in the vector DB.
 *
 * Embeds text via the AI client and upserts a vector point carrying the text
 * (and any provided metadata JSON) into the store's collection.
 *
 * @param mem            The memory store (must not be NULL).
 * @param id             Unique point id (must not be NULL).
 * @param text           Text to embed and store (must not be NULL).
 * @param metadata_json  Optional JSON object string merged with "text" (may be NULL).
 * @return 0 on success, or -1 on invalid args / embedding or upsert failure.
 */
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

    csilk_json_t* payload = metadata_json ? csilk_json_parse(metadata_json) : csilk_json_object();
    if (!payload) {
        payload = csilk_json_object();
    }
    csilk_json_add_string(payload, "text", text);

    csilk_vector_point_t pt = {
        .id = id, .vector = eres.values, .dimension = eres.dimension, .payload = payload};

    int rc = csilk_vector_db_upsert(mem->db, mem->collection, &pt, 1);
    csilk_json_free(payload);
    csilk_ai_embeddings_response_free(&eres);
    return rc;
}

/**
 * @brief Recalls the top-k most similar memories for a query string.
 *
 * Embeds the query and performs an approximate nearest-neighbour search in the
 * store's collection, writing results into the caller-provided response struct.
 *
 * @param mem    The memory store (must not be NULL).
 * @param query  Query text (must not be NULL).
 * @param limit  Maximum number of results (defaults to 5 when <= 0).
 * @param res    Output search response to populate (must not be NULL).
 * @return 0 on success, or -1 on invalid args / embedding or search failure.
 */
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

/**
 * @brief Frees a long-term agent memory store.
 *
 * Releases the embedded model name and collection strings and the store struct.
 * The underlying AI client and vector database handles are NOT freed.
 *
 * @param mem The memory store to free (may be NULL).
 */
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
