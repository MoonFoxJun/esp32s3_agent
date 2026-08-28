idf_component_register(SRCS "agent/agent_core.c" "agent/agent_tools.c" "agent/llm_client.c" "agent/lua_engine.c"
                            "hardware/serial_console.c" "hardware/serial_channel.c" "hardware/wifi_manager.c" "hardware/led_ctrl.c" "hardware/qq_channel.c" "hardware/screen.c" "hardware/gif_player.c" "hardware/display_mgr.c" "hardware/stream_player.c" "hardware/skill_store.c"
                            "tools/led_tool.c" "tools/lua_tool.c" "tools/ota_tool.c" "tools/screen_tool.c" "tools/persona_tool.c" "tools/skill_tool.c"
                            "main.c"
                    INCLUDE_DIRS "." "agent" "hardware" "tools"
                    REQUIRES nvs_flash esp_wifi esp_netif esp_http_client esp_https_ota mbedtls bootloader_support esp_driver_uart esp_lcd esp_driver_spi espressif__esp_jpeg fatfs wear_levelling)

# Lua 组件用 32 位数值类型（LUA_32BITS）编译核心；
# 我们的 lua_engine.c 必须用同样的宏编译，否则运行时
# luaL_checkversion_ 会因 LUAL_NUMSIZES 不一致而 PANIC。
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_32BITS=1)
