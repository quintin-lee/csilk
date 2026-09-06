# JSON/AI ABI Unification — Completion Record

## Completed
- Removed public value-object accessors from `include/csilk/core/json/json.h` and `src/core/json/json_access.c`.
- Restored only the public pointer-style APIs still required by code/tests: `csilk_json_object_key`, `csilk_json_object_val`, `csilk_json_copy`, `csilk_json_set_string`.
- Normalized JSON ownership transfer paths in `json_object.c`, `json_array.c`, and `json_free.c`:
  - failed insertions retain caller ownership;
  - successful insertions transfer child handle exactly once and invalidate the child wrapper;
  - borrowed views never carry owner flags and freeing a view is a safe no-op.
- Added ABI regression coverage in `tests/core/test_json_ai_abi.c` and bind-fail recovery coverage in `tests/core/test_server_bind_fail_recover.c`.
- Audited AI drivers (`openai.c`, `ollama.c`, `ai.c`) and confirmed they already use pointer/ownership JSON APIs.
- Fixed `python/tests/test_workflow_distributed.py` to use a dynamic free port, removing the localhost:8080 port-conflict timeout.
- Validated end-to-end:
  - CTest JSON/AI targets pass;
  - full C suite 228/228 passes;
  - `cmake --build build --target format`, `./scripts/check_version_sync.sh`, `git diff --check` pass;
  - Python full suite passes under `-W error::ResourceWarning`;
  - committed as `refactor(json-ai-abi): ♻️ unify JSON ownership ABI and add bind-fail recovery regression`.

## Known Limitations / Follow-ups
- No remaining active limitations from this migration stream.
- For any future JSON ABI changes, rerun `ctest --test-dir build -R 'test_json|test_json_ext|test_json_ai_abi'` plus the AI driver tests before full-suite validation.
