#include "scaffold.h"
#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_COUNT 7

extern void generate_project(const project_config_t* cfg, const char* base_path);
extern int verify_project(const project_config_t* cfg);

static void
page_identity(project_config_t* cfg, int* page, int* selected)
{
	(void)selected;
	tui_draw_header("Step 1: Project Identity");
	tui_cleanup();
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

	tui_init();
	(*page)++;
}

static void
page_template(project_config_t* cfg, int* page, int* selected)
{
	const char* templates[] = {
	    "Starter (Minimal)", "REST API (Advanced)", "AI Agent (Workflow)", "Admin Portal"};
	tui_draw_menu(templates, 4, *selected, "Step 2: Choose Template");
	tui_key_t key = tui_get_key();
	if (key == KEY_UP && *selected > 0) {
		(*selected)--;
	}
	if (key == KEY_DOWN && *selected < 3) {
		(*selected)++;
	}
	if (key == KEY_ENTER) {
		cfg->template_type = *selected;
		*selected = 0;
		(*page)++;
	}
}

static void
page_security(project_config_t* cfg, int* page, int* selected)
{
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
	tui_draw_checklist(options, flags, 6, *selected, "Step 3: Security & Performance");
	tui_key_t key = tui_get_key();
	if (key == KEY_UP && *selected > 0) {
		(*selected)--;
	}
	if (key == KEY_DOWN && *selected < 5) {
		(*selected)++;
	}
	if (key == KEY_SPACE) {
		flags[*selected] = !flags[*selected];
		cfg->has_waf = flags[0];
		cfg->has_jwt = flags[1];
		cfg->has_cors = flags[2];
		cfg->has_gzip = flags[3];
		cfg->has_recovery = flags[4];
		cfg->has_request_id = flags[5];
	}
	if (key == KEY_ENTER) {
		*selected = 0;
		(*page)++;
	}
}

static void
page_database(project_config_t* cfg, int* page, int* selected)
{
	const char* options[] = {
	    "None", "SQLite", "MySQL", "PostgreSQL", "MongoDB", "Redis (MQ/Cache)"};
	int db_sel = 0;
	if (cfg->has_sqlite) {
		db_sel = 1;
	}
	if (cfg->has_mysql) {
		db_sel = 2;
	}
	if (cfg->has_postgres) {
		db_sel = 3;
	}
	if (cfg->has_mongodb) {
		db_sel = 4;
	}
	if (cfg->has_redis) {
		db_sel = 5;
	}

	tui_draw_radiolist(options, 6, db_sel, *selected, "Step 4: Database Driver");
	tui_key_t key = tui_get_key();
	if (key == KEY_UP && *selected > 0) {
		(*selected)--;
	}
	if (key == KEY_DOWN && *selected < 5) {
		(*selected)++;
	}
	if (key == KEY_ENTER) {
		db_sel = *selected;
		cfg->has_sqlite = (db_sel == 1);
		cfg->has_mysql = (db_sel == 2);
		cfg->has_postgres = (db_sel == 3);
		cfg->has_mongodb = (db_sel == 4);
		cfg->has_redis = (db_sel == 5);
		*selected = 0;
		(*page)++;
	}
}

static void
page_vector(project_config_t* cfg, int* page, int* selected)
{
	const char* options[] = {"None", "Qdrant", "Milvus"};
	int vec_sel = cfg->has_qdrant ? 1 : (cfg->has_milvus ? 2 : 0);

	tui_draw_radiolist(options, 3, vec_sel, *selected, "Step 5: Vector Database");
	tui_key_t key = tui_get_key();
	if (key == KEY_UP && *selected > 0) {
		(*selected)--;
	}
	if (key == KEY_DOWN && *selected < 2) {
		(*selected)++;
	}
	if (key == KEY_ENTER) {
		vec_sel = *selected;
		cfg->has_qdrant = (vec_sel == 1);
		cfg->has_milvus = (vec_sel == 2);
		cfg->has_vector_db = (vec_sel > 0);
		*selected = 0;
		(*page)++;
	}
}

static void
page_features(project_config_t* cfg, int* page, int* selected)
{
	const char* options[] = {"AI Workflow Engine",
				 "Admin Dashboard",
				 "WebSocket Support",
				 "Server-Sent Events (SSE)",
				 "Prometheus Metrics",
				 "Swagger UI / OpenAPI"};
	bool flags[] = {cfg->has_workflow,
			cfg->has_admin,
			cfg->has_ws,
			cfg->has_sse,
			cfg->has_prometheus,
			cfg->has_swagger};
	tui_draw_checklist(options, flags, 6, *selected, "Step 6: Additional Features");
	tui_key_t key = tui_get_key();
	if (key == KEY_UP && *selected > 0) {
		(*selected)--;
	}
	if (key == KEY_DOWN && *selected < 5) {
		(*selected)++;
	}
	if (key == KEY_SPACE) {
		flags[*selected] = !flags[*selected];
		cfg->has_workflow = flags[0];
		cfg->has_admin = flags[1];
		cfg->has_ws = flags[2];
		cfg->has_sse = flags[3];
		cfg->has_prometheus = flags[4];
		cfg->has_swagger = flags[5];
	}
	if (key == KEY_ENTER) {
		*selected = 0;
		(*page)++;
	}
}

static void
page_verify(project_config_t* cfg, int* page, int* selected)
{
	(void)selected;
	tui_draw_header("Final Step: Verification");
	printf("Selected: %s on port %d", cfg->name, cfg->port);
	if (cfg->template_type == 0) {
		printf(", Starter");
	} else if (cfg->template_type == 1) {
		printf(", REST API");
	} else if (cfg->template_type == 2) {
		printf(", AI Agent");
	} else {
		printf(", Admin Portal");
	}
	printf("\n");

	tui_cleanup();
	if (verify_project(cfg) == 0) {
		generate_project(cfg, cfg->output_dir);
		tui_set_color(COLOR_GREEN);
		printf("\n✓ Success! Project generated in: %s\n", cfg->output_dir);
		tui_reset_color();
		printf("\nNext steps:\n  cd %s\n  mkdir build && cd build\n  cmake ..\n  make\n  "
		       "./%s\n",
		       cfg->output_dir,
		       cfg->name);
	} else {
		tui_set_color(COLOR_RED);
		printf("\n✖ Aborted: Generated code failed to compile. Please report this bug.\n");
		tui_reset_color();
	}
	(*page)++;
}

void
run_wizard(project_config_t* cfg)
{
	int page = 0;
	int selected = 0;

	while (page < PAGE_COUNT) {
		switch (page) {
		case 0:
			page_identity(cfg, &page, &selected);
			break;
		case 1:
			page_template(cfg, &page, &selected);
			break;
		case 2:
			page_security(cfg, &page, &selected);
			break;
		case 3:
			page_database(cfg, &page, &selected);
			break;
		case 4:
			page_vector(cfg, &page, &selected);
			break;
		case 5:
			page_features(cfg, &page, &selected);
			break;
		case 6:
			page_verify(cfg, &page, &selected);
			break;
		default:
			page++;
		}
	}
}
