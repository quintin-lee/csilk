#include "scaffold.h"
#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void generate_project(const project_config_t* cfg, const char* base_path);
extern int verify_project(const project_config_t* cfg);

void
run_wizard(project_config_t* cfg)
{
	int page = 0;
	int selected = 0;

	while (page < 5) {
		if (page == 0) { // Basic Info
			tui_draw_header("Step 1: Project Identity");
			tui_cleanup(); // Exit raw mode for standard input
			printf("Project Name (default: my_app): ");
			char name[64];
			fgets(name, 64, stdin);
			name[strcspn(name, "\n")] = 0;
			if (name[0]) {
				strcpy(cfg->name, name);
			} else {
				strcpy(cfg->name, "my_app");
			}

			printf("Server Port (default: 8080): ");
			char port[16];
			fgets(port, 16, stdin);
			int p = atoi(port);
			cfg->port = (p > 0) ? p : 8080;

			strcpy(cfg->output_dir, "./");
			strcat(cfg->output_dir, cfg->name);

			tui_init(); // Back to raw mode
			page++;
		} else if (page == 1) { // Template Selection
			const char* templates[] = {"Starter (Minimal)",
						   "REST API (Advanced)",
						   "AI Agent (Workflow)",
						   "Admin Portal"};
			tui_draw_menu(templates, 4, selected, "Step 2: Choose Template");
			tui_key_t key = tui_get_key();
			if (key == KEY_UP && selected > 0) {
				selected--;
			}
			if (key == KEY_DOWN && selected < 3) {
				selected++;
			}
			if (key == KEY_ENTER) {
				cfg->template_type = selected;
				page++;
				selected = 0;
			}
		} else if (page == 2) { // Security & Performance
			const char* options[] = {"WAF (Security)",
						 "JWT Auth",
						 "CORS",
						 "Gzip Compression",
						 "Recovery (Panic Handler)",
						 "Request ID"};
			bool flags[] = {cfg->has_waf,
					cfg->has_jwt,
					cfg->has_cors,
					cfg->has_gzip,
					cfg->has_recovery,
					cfg->has_request_id};
			tui_draw_checklist(
			    options, flags, 6, selected, "Step 3: Security & Performance");
			tui_key_t key = tui_get_key();
			if (key == KEY_UP && selected > 0) {
				selected--;
			}
			if (key == KEY_DOWN && selected < 5) {
				selected++;
			}
			if (key == KEY_SPACE) {
				flags[selected] = !flags[selected];
				cfg->has_waf = flags[0];
				cfg->has_jwt = flags[1];
				cfg->has_cors = flags[2];
				cfg->has_gzip = flags[3];
				cfg->has_recovery = flags[4];
				cfg->has_request_id = flags[5];
			}
			if (key == KEY_ENTER) {
				page++;
				selected = 0;
			}
		} else if (page == 3) { // Advanced & Drivers
			const char* options[] = {"AI Workflow Engine",
						 "Vector DB Support",
						 "SQLite Driver",
						 "Redis (MQ/Cache)",
						 "Prometheus Metrics",
						 "Swagger UI"};
			bool flags[] = {cfg->has_workflow,
					cfg->has_vector_db,
					cfg->has_sqlite,
					cfg->has_redis,
					cfg->has_prometheus,
					cfg->has_swagger};
			tui_draw_checklist(
			    options, flags, 6, selected, "Step 4: Drivers & Observability");
			tui_key_t key = tui_get_key();
			if (key == KEY_UP && selected > 0) {
				selected--;
			}
			if (key == KEY_DOWN && selected < 5) {
				selected++;
			}
			if (key == KEY_SPACE) {
				flags[selected] = !flags[selected];
				cfg->has_workflow = flags[0];
				cfg->has_vector_db = flags[1];
				cfg->has_sqlite = flags[2];
				cfg->has_redis = flags[3];
				cfg->has_prometheus = flags[4];
				cfg->has_swagger = flags[5];
			}
			if (key == KEY_ENTER) {
				page++;
				selected = 0;
			}
		} else if (page == 4) { // Verification
			tui_draw_header("Final Step: Verification");
			printf("Selected features: %s project with %s, %s, ...\n",
			       cfg->name,
			       cfg->has_waf ? "WAF" : "",
			       cfg->has_jwt ? "JWT" : "");
			printf("\nGenerating and Verifying (this may take a few seconds)...\n");

			tui_cleanup(); // Exit raw for log output
			if (verify_project(cfg) == 0) {
				generate_project(cfg, cfg->output_dir);
				tui_set_color(COLOR_GREEN);
				printf("\n✓ Success! Project generated in: %s\n", cfg->output_dir);
				tui_reset_color();
				printf("\nNext steps:\n  cd %s\n  mkdir build && cd build\n  cmake "
				       "..\n  make\n  ./%s\n",
				       cfg->output_dir,
				       cfg->name);
			} else {
				tui_set_color(COLOR_RED);
				printf("\n✖ Aborted: Generated code failed to compile. Please "
				       "report this bug.\n");
				tui_reset_color();
			}
			page++;
		}
	}
}
