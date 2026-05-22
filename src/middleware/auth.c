#include <string.h>

#include "gin.h"

void gin_auth_middleware(gin_ctx_t* c, gin_auth_validator_t validator) {
  const char* token = gin_get_header(c, "Authorization");
  if (!token || !validator(token)) {
    gin_status(c, 401);
    gin_abort(c);
  }
}
