#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 注册 ota_update 工具：让 LLM 在用户明确要求时触发固件升级
 * （需要提供固件 .bin 的 https 下载地址）
 */
esp_err_t ota_tool_register(void);

#ifdef __cplusplus
}
#endif
