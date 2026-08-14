/**
 * @file wf_ai_agents.c
 * @brief Agent node types: ReAct, Reflexion, HITL, worker, and task dispatcher.
 *
 * @copyright MIT License
 */

#include "workflow_internal.h"
#include "wf_ai_internal.h"
#include "csilk/core/sync.h"

/* --- ReAct Agent --- */

/* Frees a copied csilk_agent_react_config_t (model/system_prompt/prompt strings). */
static void
agent_react_config_free(void* ptr)
{
    csilk_agent_react_config_t* c = (csilk_agent_react_config_t*)ptr;
    free((void*)c->model);
    free((void*)c->system_prompt);
    free((void*)c->prompt);
    free(c);
}

/* ReAct node handler: runs the Thought/Action/Observation loop via ai_node_handler. */
static csilk_data_t*
agent_react_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)input;
    csilk_agent_react_config_t* config = (csilk_agent_react_config_t*)user_data;
    if (!config) {
        return NULL;
    }
    const char* default_sys = "You are an AI ReAct Reasoning Agent. Follow the Thought, Action, "
                              "ActionInput, Observation cycle to solve the problem.";
    csilk_ai_config_t ai_cfg = {
        .model = config->model ? config->model : "gpt-3.5-turbo",
        .system_msg = config->system_prompt ? config->system_prompt : default_sys,
        .prompt = config->prompt ? config->prompt : "{{node.value}}",
        .temperature = config->temperature > 0 ? config->temperature : 0.7,
        .max_tokens = config->max_tokens > 0 ? config->max_tokens : 1024,
        .stream = 0,
        .max_history_messages = config->max_iterations > 0 ? config->max_iterations * 2 : 20};
    return ai_node_handler(ctx, input, &ai_cfg);
}

/**
 * @brief Adds a ReAct reasoning agent node to the workflow.
 *
 * Copies the supplied configuration (duplicating its string fields) and wraps
 * it in a node whose handler drives the Thought/Action/ActionInput/Observation
 * cycle through the AI chat backend. The copy is freed via the node's
 * user_data_free hook on csilk_wf_free().
 *
 * @param wf     The workflow instance (must not be NULL).
 * @param id     Unique node identifier (must not be NULL).
 * @param config ReAct configuration (must not be NULL).
 * @return The new node pointer, or NULL on invalid args / allocation failure.
 */
csilk_wf_node_t*
csilk_wf_add_agent_react(csilk_wf_t* wf, const char* id, const csilk_agent_react_config_t* config)
{
    if (!wf || !id || !config) {
        return NULL;
    }
    csilk_agent_react_config_t* copy = malloc(sizeof(csilk_agent_react_config_t));
    memcpy(copy, config, sizeof(csilk_agent_react_config_t));
    copy->model = config->model ? strdup(config->model) : NULL;
    copy->system_prompt = config->system_prompt ? strdup(config->system_prompt) : NULL;
    copy->prompt = config->prompt ? strdup(config->prompt) : NULL;

    csilk_wf_node_t* node = csilk_wf_add(wf, id, agent_react_node_handler, copy);
    if (node) {
        node->user_data_free = agent_react_config_free;
    }
    return node;
}

/* --- Reflexion Agent --- */

/* Frees a copied csilk_agent_reflexion_config_t (model/prompt strings). */
static void
agent_reflexion_config_free(void* ptr)
{
    csilk_agent_reflexion_config_t* c = (csilk_agent_reflexion_config_t*)ptr;
    free((void*)c->model);
    free((void*)c->prompt);
    free(c);
}

/* Reflexion node handler: retries the model and applies an eval_fn feedback
 * loop until the evaluator passes or max_reflections is reached. */
static csilk_data_t*
agent_reflexion_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    csilk_agent_reflexion_config_t* config = (csilk_agent_reflexion_config_t*)user_data;
    if (!config) {
        return NULL;
    }
    int           max_reflections = config->max_reflections > 0 ? config->max_reflections : 3;
    int           reflection_count = 0;
    csilk_data_t* current_output = NULL;

    while (reflection_count < max_reflections) {
        reflection_count++;
        csilk_ai_config_t ai_cfg = {
            .model = config->model ? config->model : "gpt-3.5-turbo",
            .system_msg = "You are an AI Agent with Reflexion and Self-Correction capability.",
            .prompt = config->prompt ? config->prompt : "{{node.value}}",
            .temperature = 0.7,
            .max_tokens = 1024,
            .stream = 0,
            .max_history_messages = 10};
        current_output = ai_node_handler(ctx, input, &ai_cfg);
        if (!current_output || !current_output->value) {
            break;
        }

        if (config->eval_fn) {
            char* feedback = NULL;
            int   pass = config->eval_fn(
                (const char*)current_output->value, &feedback, config->eval_user_data);
            if (pass) {
                if (feedback) {
                    free(feedback);
                }
                break;
            } else {
                if (feedback) {
                    csilk_wf_ctx_set_memory(ctx, "reflexion_feedback", feedback);
                    free(feedback);
                }
            }
        } else {
            break;
        }
    }
    return current_output;
}

/**
 * @brief Adds a Reflexion (self-correcting) agent node to the workflow.
 *
 * Copies the configuration and wraps it in a node whose handler iteratively
 * queries the model and, when an eval_fn is provided, feeds its feedback back
 * as context memory ("reflexion_feedback") until the evaluator passes or the
 * reflection budget is exhausted.
 *
 * @param wf     The workflow instance (must not be NULL).
 * @param id     Unique node identifier (must not be NULL).
 * @param config Reflexion configuration (must not be NULL).
 * @return The new node pointer, or NULL on invalid args / allocation failure.
 */
csilk_wf_node_t*
csilk_wf_add_agent_reflexion(csilk_wf_t*                           wf,
                             const char*                           id,
                             const csilk_agent_reflexion_config_t* config)
{
    if (!wf || !id || !config) {
        return NULL;
    }
    csilk_agent_reflexion_config_t* copy = malloc(sizeof(csilk_agent_reflexion_config_t));
    memcpy(copy, config, sizeof(csilk_agent_reflexion_config_t));
    copy->model = config->model ? strdup(config->model) : NULL;
    copy->prompt = config->prompt ? strdup(config->prompt) : NULL;

    csilk_wf_node_t* node = csilk_wf_add(wf, id, agent_reflexion_node_handler, copy);
    if (node) {
        node->user_data_free = agent_reflexion_config_free;
    }
    return node;
}

/* --- Multi-Agent Task Dispatcher --- */

/**
 * @brief Publishes a task to a workflow's message-queue topic.
 *
 * Serializes task_json and publishes it on the given topic through the
 * workflow's bound message queue (see csilk_wf_enable_distributed()).
 *
 * @param wf        The workflow instance whose MQ is used.
 * @param topic     Target MQ topic (must not be NULL).
 * @param task_json Null-terminated task JSON payload (must not be NULL).
 * @return 0 on success, or -1 if wf, wf->mq, topic, or task_json is NULL.
 */
int
csilk_agent_publish_task(csilk_wf_t* wf, const char* topic, const char* task_json)
{
    if (!wf || !wf->mq || !topic || !task_json) {
        return -1;
    }
    return csilk_mq_publish(wf->mq, topic, task_json, strlen(task_json));
}

/* --- Worker Agent --- */

typedef struct {
    char*              topic;
    csilk_wf_handler_t handler;
    void*              user_data;
} worker_config_t;

/* Frees a worker_config_t (topic string). */
static void
worker_config_free(void* ptr)
{
    worker_config_t* wc = (worker_config_t*)ptr;
    free(wc->topic);
    free(wc);
}

/* Worker node handler: dispatches to the user-supplied handler callback. */
static csilk_data_t*
agent_worker_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    worker_config_t* wc = (worker_config_t*)user_data;
    if (!wc || !wc->handler) {
        return NULL;
    }
    return wc->handler(ctx, input, wc->user_data);
}

/**
 * @brief Adds a worker agent node that subscribes to a task topic.
 *
 * Creates a node bound to a dispatch topic; the node's handler simply forwards
 * the input to the caller-supplied handler callback. The topic string is owned
 * by the node and freed via the user_data_free hook.
 *
 * @param wf        The workflow instance (must not be NULL).
 * @param id        Unique node identifier (must not be NULL).
 * @param topic     Task topic this worker consumes (must not be NULL).
 * @param handler   Node execution callback (must not be NULL).
 * @param user_data Opaque pointer passed to handler.
 * @return The new node pointer, or NULL on invalid args / allocation failure.
 */
csilk_wf_node_t*
csilk_wf_add_agent_worker(
    csilk_wf_t* wf, const char* id, const char* topic, csilk_wf_handler_t handler, void* user_data)
{
    if (!wf || !id || !topic || !handler) {
        return NULL;
    }
    worker_config_t* wc = malloc(sizeof(worker_config_t));
    wc->topic = strdup(topic);
    wc->handler = handler;
    wc->user_data = user_data;

    csilk_wf_node_t* node = csilk_wf_add(wf, id, agent_worker_node_handler, wc);
    if (node) {
        node->user_data_free = worker_config_free;
    }
    return node;
}

/* --- Human-in-the-Loop (HITL) Agent --- */

/* Frees a copied csilk_agent_hitl_config_t (model/prompt strings). */
static void
agent_hitl_config_free(void* ptr)
{
    csilk_agent_hitl_config_t* c = (csilk_agent_hitl_config_t*)ptr;
    free((void*)c->model);
    free((void*)c->prompt);
    free(c);
}

/* HITL node handler: queries the model, then records an eval_fn decision in
 * context memory under "hitl_status" (requires_approval/approved/rejected). */
static csilk_data_t*
agent_hitl_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    csilk_agent_hitl_config_t* config = (csilk_agent_hitl_config_t*)user_data;
    if (!config) {
        return NULL;
    }
    csilk_ai_config_t ai_cfg = {
        .model = config->model ? config->model : "gpt-3.5-turbo",
        .system_msg = "You are an AI Agent operating under Human-in-the-Loop supervision.",
        .prompt = config->prompt ? config->prompt : "{{node.value}}",
        .temperature = 0.7,
        .max_tokens = 1024,
        .stream = 0,
        .max_history_messages = 10};
    csilk_data_t* output = ai_node_handler(ctx, input, &ai_cfg);
    if (!output || !output->value) {
        return output;
    }

    if (config->eval_fn) {
        csilk_hitl_decision_t decision =
            config->eval_fn((const char*)output->value, config->eval_user_data);
        if (decision == CSILK_HITL_REQUIRES_HUMAN) {
            csilk_wf_ctx_set_memory(ctx, "hitl_status", "requires_approval");
        } else if (decision == CSILK_HITL_APPROVED) {
            csilk_wf_ctx_set_memory(ctx, "hitl_status", "approved");
        } else {
            csilk_wf_ctx_set_memory(ctx, "hitl_status", "rejected");
        }
    }
    return output;
}

/**
 * @brief Adds a Human-in-the-Loop (HITL) agent node to the workflow.
 *
 * Copies the configuration and wraps it in a node that queries the model, then
 * invokes eval_fn to classify the output (requires-human / approved / rejected),
 * recording the verdict in context memory as "hitl_status". The node is marked
 * interactive so the scheduler pauses for approval before executing it.
 *
 * @param wf     The workflow instance (must not be NULL).
 * @param id     Unique node identifier (must not be NULL).
 * @param config HITL configuration (must not be NULL).
 * @return The new node pointer, or NULL on invalid args / allocation failure.
 */
csilk_wf_node_t*
csilk_wf_add_agent_hitl(csilk_wf_t* wf, const char* id, const csilk_agent_hitl_config_t* config)
{
    if (!wf || !id || !config) {
        return NULL;
    }
    csilk_agent_hitl_config_t* copy = malloc(sizeof(csilk_agent_hitl_config_t));
    memcpy(copy, config, sizeof(csilk_agent_hitl_config_t));
    copy->model = config->model ? strdup(config->model) : NULL;
    copy->prompt = config->prompt ? strdup(config->prompt) : NULL;

    csilk_wf_node_t* node = csilk_wf_add(wf, id, agent_hitl_node_handler, copy);
    if (node) {
        node->user_data_free = agent_hitl_config_free;
        csilk_wf_node_set_interactive(node, 1);
    }
    return node;
}
