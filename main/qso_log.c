/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "qso_log.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#define MOUNT_POINT "/qso"
#define LOG_FILE    MOUNT_POINT "/ft8.adi"   // records only; the ADIF header is added on download

static const char *TAG = "qso_log";
static bool s_ready;
static unsigned s_count;
static size_t s_bytes;

esp_err_t qso_log_init(void)
{
    const esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = true,   // the partition was never used before this feature
        .max_files = 2,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    wl_handle_t wl;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, "storage", &config, &wl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "storage partition not mounted (%s): the QSO log is off", esp_err_to_name(err));
        return err;
    }
    // One record per line: count them once here, then keep the count up to date.
    FILE *f = fopen(LOG_FILE, "r");
    if (f) {
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            s_bytes += n;
            for (size_t i = 0; i < n; ++i) s_count += buf[i] == '\n';
        }
        fclose(f);
    }
    s_ready = true;
    ESP_LOGI(TAG, "QSO log: %u QSOs, %u bytes", s_count, (unsigned)s_bytes);
    return ESP_OK;
}

bool qso_log_ready(void)
{
    return s_ready;
}

esp_err_t qso_log_append(const char *record)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    const size_t len = strlen(record);
    if (s_bytes + len > QSO_LOG_MAX_BYTES) return ESP_ERR_NO_MEM;
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return ESP_FAIL;
    // Flush and sync before answering, so a QSO the page reports as logged survives a power cut.
    bool ok = fwrite(record, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        // Cut off a partly written record, so the next one still starts on its own line.
        if (truncate(LOG_FILE, (off_t)s_bytes) != 0) {
            struct stat st;
            if (stat(LOG_FILE, &st) == 0) s_bytes = (size_t)st.st_size;
        }
        return ESP_FAIL;
    }
    s_bytes += len;
    ++s_count;
    return ESP_OK;
}

void qso_log_stats(unsigned *count, size_t *bytes)
{
    *count = s_count;
    *bytes = s_bytes;
}

FILE *qso_log_open(void)
{
    return s_ready && s_bytes ? fopen(LOG_FILE, "r") : NULL;
}
