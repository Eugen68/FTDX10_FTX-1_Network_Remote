/*
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "https_identity.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"

#define IDENTITY_NAMESPACE "https_identity"
#define IDENTITY_KEY "pair_v1"

typedef struct {
    uint16_t cert_len;
    uint16_t key_len;
    uint8_t cert[1024];
    uint8_t key[256];
} https_pair_t;

static const char *TAG = "https_identity";
static bool s_regenerate;
// Published before the HTTPS task starts, immutable for the lifetime of that server.
static esp_ip4_addr_t s_cert_ip;
static char s_cert_fingerprint[32 * 3];

static esp_ip4_addr_t current_ipv4(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        return info.ip;
    }
    return (esp_ip4_addr_t){0};
}

bool https_identity_covers_ip(void)
{
    esp_ip4_addr_t ip = current_ipv4();
    return ip.addr != 0 && ip.addr == s_cert_ip.addr;
}

const char *https_identity_fingerprint(void)
{
    return s_cert_fingerprint;
}

void https_identity_request_regeneration(void)
{
    s_regenerate = true;
}

static bool pair_valid(const https_pair_t *pair)
{
    if (pair->cert_len == 0 || pair->cert_len > sizeof(pair->cert) ||
        pair->key_len == 0 || pair->key_len > sizeof(pair->key)) {
        return false;
    }

    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&key);
    bool valid = mbedtls_x509_crt_parse_der(&cert, pair->cert, pair->cert_len) == 0 &&
                 mbedtls_pk_parse_key(&key, pair->key, pair->key_len, NULL, 0) == 0 &&
                 mbedtls_pk_check_pair(&cert.pk, &key) == 0;
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&cert);
    return valid;
}

static esp_err_t load_pair(https_pair_t *pair)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IDENTITY_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = sizeof(*pair);
    err = nvs_get_blob(handle, IDENTITY_KEY, pair, &len);
    nvs_close(handle);
    if (err == ESP_OK && (len != sizeof(*pair) || !pair_valid(pair))) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

static esp_err_t save_pair(const https_pair_t *pair)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    // Store both DER objects together so a power cut cannot mix two identities.
    err = nvs_set_blob(handle, IDENTITY_KEY, pair, sizeof(*pair));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static int generate_pair(https_pair_t *pair, esp_ip4_addr_t ip)
{
    static unsigned char fqdn[] = "trx-remote.local";
    static unsigned char hostname[] = "trx-remote";
    // ESP-IDF stores IPv4 addresses in network byte order. Mbed TLS expects
    // four raw octets for an iPAddress SAN.
    unsigned char ip_bytes[4];
    memcpy(ip_bytes, &ip.addr, sizeof(ip_bytes));
    mbedtls_x509_san_list names[] = {
        { .node = { .type = MBEDTLS_X509_SAN_DNS_NAME,
                    .san.unstructured_name = { .p = fqdn, .len = sizeof(fqdn) - 1 } } },
        { .node = { .type = MBEDTLS_X509_SAN_DNS_NAME,
                    .san.unstructured_name = { .p = hostname, .len = sizeof(hostname) - 1 } } },
        { .node = { .type = MBEDTLS_X509_SAN_IP_ADDRESS,
                    .san.unstructured_name = { .p = ip_bytes, .len = sizeof(ip_bytes) } } },
    };
    names[0].next = &names[1];
    names[1].next = &names[2];
    const char *subject = "O=FTDX10 FTX-1 Network Remote,CN=trx-remote.local";

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_pk_context key;
    mbedtls_x509write_cert cert;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&cert);
    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    int ret = psa_generate_key(&attributes, &key_id);
    psa_reset_key_attributes(&attributes);
    if (ret != 0) {
        goto done;
    }
    ret = mbedtls_pk_copy_from_psa(key_id, &key);
    psa_destroy_key(key_id);
    if (ret != 0) {
        goto done;
    }

    uint8_t serial[16];
    ret = psa_generate_random(serial, sizeof(serial));
    if (ret != 0) {
        goto done;
    }
    serial[0] = (serial[0] & 0x7f) | 0x40;

    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert, &key);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key);
    static unsigned char server_auth_oid[] = MBEDTLS_OID_SERVER_AUTH;
    const mbedtls_asn1_sequence server_auth = {
        .buf = { .tag = MBEDTLS_ASN1_OID, .p = server_auth_oid, .len = sizeof(server_auth_oid) - 1 },
    };
    // A self-signed identity installed as a local CA also needs the CA bit
    // and keyCertSign for Android's CA certificate installer.
    // Derive validity from the firmware build so first boot works offline.
    if ((ret = mbedtls_x509write_crt_set_validity(&cert, HTTPS_CERT_NOT_BEFORE, HTTPS_CERT_NOT_AFTER)) != 0 ||
        (ret = mbedtls_x509write_crt_set_serial_raw(&cert, serial, sizeof(serial))) != 0 ||
        (ret = mbedtls_x509write_crt_set_subject_name(&cert, subject)) != 0 ||
        (ret = mbedtls_x509write_crt_set_issuer_name(&cert, subject)) != 0 ||
        (ret = mbedtls_x509write_crt_set_basic_constraints(&cert, 1, 0)) != 0 ||
        (ret = mbedtls_x509write_crt_set_key_usage(&cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                                                 MBEDTLS_X509_KU_KEY_CERT_SIGN)) != 0 ||
        (ret = mbedtls_x509write_crt_set_ext_key_usage(&cert, &server_auth)) != 0 ||
        (ret = mbedtls_x509write_crt_set_subject_alternative_name(&cert, names)) != 0) {
        goto done;
    }
    ret = mbedtls_x509write_crt_der(&cert, pair->cert, sizeof(pair->cert));
    if (ret <= 0) {
        goto done;
    }
    pair->cert_len = ret;
    memmove(pair->cert, pair->cert + sizeof(pair->cert) - ret, ret);

    ret = mbedtls_pk_write_key_der(&key, pair->key, sizeof(pair->key));
    if (ret <= 0) {
        goto done;
    }
    pair->key_len = ret;
    memmove(pair->key, pair->key + sizeof(pair->key) - ret, ret);
    mbedtls_platform_zeroize(pair->key + ret, sizeof(pair->key) - ret);
    memset(pair->cert + pair->cert_len, 0, sizeof(pair->cert) - pair->cert_len);
    ret = 0;

done:
    mbedtls_x509write_crt_free(&cert);
    mbedtls_pk_free(&key);
    return ret;
}

typedef struct {
    https_pair_t *pair;
    esp_ip4_addr_t ip;
    SemaphoreHandle_t finished;
    int result;
} https_generation_t;

static void generation_task(void *arg)
{
    https_generation_t *job = arg;
    int64_t start = esp_timer_get_time();
    job->result = generate_pair(job->pair, job->ip);
    ESP_LOGI(TAG, "HTTPS key and certificate generation took %" PRId64 " ms (result %d)",
             (esp_timer_get_time() - start + 500) / 1000, job->result);
    xSemaphoreGive(job->finished);
    vTaskDelete(NULL);
}

static void try_generate_pair(https_pair_t *pair, esp_ip4_addr_t ip)
{
    https_generation_t job = { .pair = pair, .ip = ip, .result = ESP_ERR_NO_MEM };
    job.finished = xSemaphoreCreateBinary();
    if (job.finished) {
        // Same priority as the watchdog-monitored idle tasks, which still get time slices.
        if (xTaskCreate(generation_task, "https_keygen", 6144, &job, tskIDLE_PRIORITY, NULL) == pdPASS) {
            xSemaphoreTake(job.finished, portMAX_DELAY);
        }
        vSemaphoreDelete(job.finished);
    }
    int ret = job.result;

    if (ret != 0) {
        ESP_LOGW(TAG, "Certificate generation failed (%d)", ret);
    } else if (!pair_valid(pair)) {
        ESP_LOGW(TAG, "Generated HTTPS identity failed validation");
    } else {
        esp_err_t err = save_pair(pair);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "New device HTTPS certificate saved to NVS");
            return;
        }
        ESP_LOGW(TAG, "Could not save HTTPS identity: %s", esp_err_to_name(err));
    }

    // A failed rotation must keep using the old identity if it is readable.
    if (load_pair(pair) != ESP_OK) {
        pair->cert_len = 0;
    }
}

static void record_certificate(const httpd_ssl_config_t *config)
{
    mbedtls_x509_crt cert;
    mbedtls_x509_crt_init(&cert);
    s_cert_ip.addr = 0;
    s_cert_fingerprint[0] = '\0';
    if (mbedtls_x509_crt_parse(&cert, config->servercert, config->servercert_len) == 0) {
        uint8_t hash[32];
        size_t hash_len;
        if (psa_hash_compute(PSA_ALG_SHA_256, cert.raw.p, cert.raw.len,
                             hash, sizeof(hash), &hash_len) == PSA_SUCCESS) {
            for (size_t i = 0; i < hash_len; ++i) {
                snprintf(s_cert_fingerprint + 3 * i, 4, "%02X%s",
                         (unsigned)hash[i], i + 1 == hash_len ? "" : ":");
            }
        }
        for (const mbedtls_x509_sequence *name = &cert.subject_alt_names; name; name = name->next) {
            const mbedtls_x509_buf *entry = &name->buf;
            if (entry->tag == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_X509_SAN_IP_ADDRESS) &&
                entry->len == sizeof(s_cert_ip.addr)) {
                memcpy(&s_cert_ip.addr, entry->p, sizeof(s_cert_ip.addr));
                break;
            }
        }
    }
    mbedtls_x509_crt_free(&cert);
}

static esp_err_t start_with_certificate(httpd_handle_t *server, httpd_ssl_config_t *config,
                                        const char *source)
{
    record_certificate(config);
    // The SDK takes copies of the certificate and key buffers.
    esp_err_t err = httpd_ssl_start(server, config);
    if (err != ESP_OK) {
        return err;
    }
    if (s_cert_fingerprint[0]) {
        ESP_LOGI(TAG, "Active HTTPS certificate (%s) SHA-256: %s", source, s_cert_fingerprint);
    } else {
        ESP_LOGW(TAG, "Unable to report the active HTTPS certificate fingerprint");
    }
    esp_ip4_addr_t ip = current_ipv4();
    if (ip.addr != 0 && ip.addr != s_cert_ip.addr) {
        ESP_LOGW(TAG, "HTTPS certificate does not cover current IP " IPSTR "; use menu c to regenerate",
                 IP2STR(&ip));
    }
    return ESP_OK;
}

esp_err_t https_identity_start(httpd_handle_t *server, httpd_ssl_config_t *config)
{
    // Wi-Fi is already up, so the hardware random source has RF entropy.
    https_pair_t *pair = calloc(1, sizeof(*pair));
    if (pair) {
        esp_err_t err = load_pair(pair);
        bool use_stored = err == ESP_OK;
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Could not load HTTPS identity: %s", esp_err_to_name(err));
        }
        if (err == ESP_ERR_NVS_NOT_FOUND || s_regenerate) {
            esp_ip4_addr_t ip = current_ipv4();
            if (ip.addr != 0) {
                try_generate_pair(pair, ip);
                use_stored = pair->cert_len != 0;
            } else {
                ESP_LOGW(TAG, "Current IPv4 address unavailable; skipping certificate generation");
            }
        }
        if (use_stored) {
            httpd_ssl_config_t device_config = *config;
            device_config.servercert = pair->cert;
            device_config.servercert_len = pair->cert_len;
            device_config.prvtkey_pem = pair->key;
            device_config.prvtkey_len = pair->key_len;
            err = start_with_certificate(server, &device_config, "device NVS");
            mbedtls_platform_zeroize(pair, sizeof(*pair));
            free(pair);
            return err;
        }
        mbedtls_platform_zeroize(pair, sizeof(*pair));
        free(pair);
    }
    ESP_LOGW(TAG, "Device HTTPS identity unavailable; using embedded fallback certificate");
    return start_with_certificate(server, config, "embedded fallback");
}
