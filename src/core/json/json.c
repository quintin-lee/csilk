/**
 * @file src/core/json/json.c
 * @brief JSON module - thin wrapper for backward compatibility.
 *
 * Implementation split across:
 *   json_internal.c   - view ring and helper functions
 *   json_factory.c    - object/array/string/number factories
 *   json_object.c     - object mutation (add_*)
 *   json_array.c      - array operations
 *   json_access.c     - get-by-key and type extractors
 *   json_type.c       - type predicates
 *   json_parse.c      - JSON parsing
 *   json_serialize.c  - serialization
 *   json_free.c       - memory management
 *   json_copy.c       - deep copy
 *   json_iterate.c    - key/value iteration
 *   json_mutate.c     - mutation helpers
 */

#include "json_internal.h"
