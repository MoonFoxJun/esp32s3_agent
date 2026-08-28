#include "skill_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "skill_store";

#define STORAGE_MOUNT "/storage"
#define SKILLS_DIR    "/storage/skills"
#define META_FILE     "SKILL.md"
#define SCRIPT_FILE   "script.lua"
#define MAX_NAME_LEN  24

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_ready = false;

bool skill_store_ready(void)
{
    return s_ready;
}

esp_err_t skill_store_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* 挂载 storage 分区为 FATFS（首次启动会自动格式化）*/
    esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("storage", STORAGE_MOUNT,
                                                     &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount /storage failed: %s", esp_err_to_name(err));
        return err;
    }

    mkdir(SKILLS_DIR, 0755);   /* 首次挂载后目录是空的，建出来 */
    s_ready = true;
    ESP_LOGI(TAG, "skill store ready: %s", SKILLS_DIR);
    return ESP_OK;
}

/* 技能名合法性：只允许字母/数字/下划线（防止路径穿越）*/
static bool valid_name(const char *name)
{
    if (!name || name[0] == '\0' || strlen(name) > MAX_NAME_LEN) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

static void skill_dir_path(const char *name, char *out, size_t sz)
{
    snprintf(out, sz, SKILLS_DIR "/%s", name);
}

static void skill_file_path(const char *name, const char *file, char *out, size_t sz)
{
    snprintf(out, sz, SKILLS_DIR "/%s/%s", name, file);
}

esp_err_t skill_store_save(const char *name, const char *description, const char *script)
{
    if (!valid_name(name) || !script || script[0] == '\0' || !s_ready) {
        return ESP_ERR_INVALID_ARG;
    }

    char dir[128], meta[160], scr[160];
    skill_dir_path(name, dir, sizeof(dir));
    skill_file_path(name, META_FILE, meta, sizeof(meta));
    skill_file_path(name, SCRIPT_FILE, scr, sizeof(scr));

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s failed: %s", dir, strerror(errno));
        return ESP_FAIL;
    }

    /* SKILL.md：name 一行 + description 一行（skill_list 读 description）*/
    FILE *f = fopen(meta, "w");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", meta);
        return ESP_FAIL;
    }
    fprintf(f, "name: %s\ndescription: %s\n", name, description ? description : "");
    fclose(f);

    f = fopen(scr, "w");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", scr);
        return ESP_FAIL;
    }
    fwrite(script, 1, strlen(script), f);
    fclose(f);

    ESP_LOGI(TAG, "skill saved: %s (%d B script)", name, (int)strlen(script));
    return ESP_OK;
}

esp_err_t skill_store_load_script(const char *name, char *buf, size_t sz)
{
    if (!valid_name(name) || !buf || sz == 0 || !s_ready) {
        return ESP_ERR_INVALID_ARG;
    }

    char scr[160];
    skill_file_path(name, SCRIPT_FILE, scr, sizeof(scr));
    FILE *f = fopen(scr, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t got = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[got] = '\0';
    return got > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t skill_store_list(char *out, size_t out_sz)
{
    if (!out || out_sz == 0 || !s_ready) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    DIR *d = opendir(SKILLS_DIR);
    if (!d) {
        return ESP_FAIL;
    }

    size_t pos = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && pos < out_sz - 96) {
        if (e->d_name[0] == '.') {
            continue;   /* . 和 .. */
        }
        /* 读 SKILL.md 里的 description 行 */
        char meta[160];
        skill_file_path(e->d_name, META_FILE, meta, sizeof(meta));
        char desc[128] = "";
        FILE *f = fopen(meta, "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "description: ", 13) == 0) {
                    strlcpy(desc, line + 13, sizeof(desc));
                    char *nl = strchr(desc, '\n');
                    if (nl) {
                        *nl = '\0';
                    }
                    break;
                }
            }
            fclose(f);
        }
        pos += snprintf(out + pos, out_sz - pos, "- %s: %s\n", e->d_name, desc);
    }
    closedir(d);
    return ESP_OK;
}

esp_err_t skill_store_delete(const char *name)
{
    if (!valid_name(name) || !s_ready) {
        return ESP_ERR_INVALID_ARG;
    }

    char meta[160], scr[160], dir[128];
    skill_file_path(name, META_FILE, meta, sizeof(meta));
    skill_file_path(name, SCRIPT_FILE, scr, sizeof(scr));
    skill_dir_path(name, dir, sizeof(dir));

    unlink(meta);
    unlink(scr);
    if (rmdir(dir) != 0) {
        ESP_LOGW(TAG, "rmdir %s failed", dir);
    }
    ESP_LOGI(TAG, "skill deleted: %s", name);
    return ESP_OK;
}
