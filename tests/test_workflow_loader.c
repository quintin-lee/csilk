#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"

csilk_data_t* t_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input,
                        void* user_data) {
  (void)ctx;
  (void)input;
  (void)user_data;
  printf("[LoaderTest] Executing dummy handler\n");
  return NULL;
}

void test_workflow_json_loader() {
  printf("Testing workflow JSON loader...\n");

  csilk_wf_register_handler("dummy", t_handler);

  const char* json =
      "{"
      "\"name\": \"TestWF\","
      "\"steps\": ["
      "  {\"id\": \"step1\", \"handler\": \"dummy\", \"entry\": true},"
      "  {\"id\": \"step2\", \"type\": \"ai\", \"config\": {\"model\": "
      "\"gpt-4\", \"prompt\": \"hello\"}}"
      "],"
      "\"connections\": ["
      "  {\"from\": \"step1\", \"to\": \"step2\"}"
      "]"
      "}";

  csilk_wf_t* wf = csilk_wf_from_json(json);
  assert(wf != NULL);

  char* mermaid = csilk_wf_to_mermaid(wf);
  assert(mermaid != NULL);
  assert(strstr(mermaid, "\"step1\" --> \"step2\"") != NULL);

  free(mermaid);
  csilk_wf_free(wf);
  printf("test_workflow_json_loader: PASS\n");
}

int main() {
  test_workflow_json_loader();
  return 0;
}
