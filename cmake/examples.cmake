# cmake/examples.cmake — example executables

add_executable(example_server examples/basic/example_server.c)
target_link_libraries(example_server csilk)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/examples/advanced/config_multi.yaml ${CMAKE_CURRENT_BINARY_DIR}/config.yaml COPYONLY)

add_executable(example_app examples/basic/example_app.c)
target_link_libraries(example_app csilk)

add_executable(example_ai examples/ai/example_ai.c)
target_link_libraries(example_ai csilk)

add_executable(example_ai_providers examples/ai/example_ai_providers.c)
target_link_libraries(example_ai_providers csilk)

add_executable(example_ai_workflow examples/ai/example_ai_workflow.c)
target_link_libraries(example_ai_workflow csilk)

add_executable(example_workflow_ui examples/ai/example_workflow_ui.c)
target_link_libraries(example_workflow_ui csilk)

add_executable(example_db examples/database/example_db.c)
target_link_libraries(example_db csilk)

add_executable(example_ws_tls_mq examples/websocket/example_ws_tls_mq.c)
target_link_libraries(example_ws_tls_mq csilk)

add_executable(example_websocket examples/websocket/example_websocket.c)
target_link_libraries(example_websocket csilk)

add_executable(example_sse examples/middleware/example_sse.c)
target_link_libraries(example_sse csilk)

add_executable(example_tls examples/advanced/example_tls.c)
target_link_libraries(example_tls csilk)

add_executable(example_custom_driver examples/advanced/example_custom_driver.c)
target_link_libraries(example_custom_driver csilk)
