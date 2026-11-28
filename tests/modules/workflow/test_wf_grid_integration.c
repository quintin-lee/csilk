#include <assert.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/workflow/workflow.h"
#include "csilk/workflow/grid.h"

int
main(void)
{
    csilk_wf_t* wf = csilk_wf_new("test_grid_wf");
    assert(wf != NULL);
    assert(csilk_wf_get_grid(wf) == NULL);

    csilk_wf_grid_t* grid = csilk_wf_grid_new("node-wf-1", NULL, NULL);
    assert(grid != NULL);

    int rc = csilk_wf_set_grid(wf, grid);
    assert(rc == 0);
    assert(csilk_wf_get_grid(wf) == grid);

    rc = csilk_wf_submit_grid_task(wf, "wf-task-1", "{\"action\":\"map\"}", 16);
    assert(rc == 0);

    char task_id[64] = {0};
    rc = csilk_wf_poll_grid_task(wf, task_id, sizeof(task_id));
    assert(rc == 0);
    assert(strcmp(task_id, "wf-task-1") == 0);

    /* Empty poll returns -1 */
    rc = csilk_wf_poll_grid_task(wf, task_id, sizeof(task_id));
    assert(rc == -1);

    csilk_wf_free(wf);
    csilk_wf_grid_free(grid);
    printf("test_wf_grid_integration passed successfully\n");
    return 0;
}
