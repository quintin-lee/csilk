#ifndef CSILK_PERM_H
#define CSILK_PERM_H

typedef struct csilk_ctx_s csilk_ctx_t;

struct csilk_perm_driver_s {
  const char* name;
  int (*check)(csilk_ctx_t* c, const char* permission, const char* resource);
};

typedef struct csilk_perm_driver_s csilk_perm_driver_t;

typedef struct {
  const char* role;
  const char* permission;
  const char* resource;
} csilk_perm_rule_t;

void csilk_perm_init(void);

int csilk_perm_register_driver(const char* name, csilk_perm_driver_t* driver);

csilk_perm_driver_t* csilk_perm_get_driver(const char* name);

int csilk_perm_set_default(const char* name);

int csilk_perm_check(csilk_ctx_t* c, const char* permission,
                     const char* resource);

void csilk_perm_require(csilk_ctx_t* c, const char* permission,
                        const char* resource);

void csilk_perm_simple_init(void);

int csilk_perm_simple_allow(const char* role, const char* permission,
                            const char* resource);

#endif
