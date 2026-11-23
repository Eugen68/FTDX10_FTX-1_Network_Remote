/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_tls.h"
#include "lwip/sockets.h"
#include "mbedtls/ssl.h"
#include "web_session_diag.h"

static const char *TAG = "web_session";

typedef struct {
    unsigned id, requests;
    char first[48];
    char last[48];
    const char *close_reason;
    httpd_recv_func_t recv;
} session_t;

static session_t s_sessions[CONFIG_LWIP_MAX_SOCKETS];
static atomic_uint s_opened, s_closed, s_purges, s_handshake_errors;
static _Thread_local bool s_queueing_lru, s_closing_lru;

static session_t *slot_for_fd(int fd)
{
    const int slot = fd - LWIP_SOCKET_OFFSET;
    return slot >= 0 && slot < CONFIG_LWIP_MAX_SOCKETS ? &s_sessions[slot] : NULL;
}

web_session_stats_t web_session_stats(void)
{
    return (web_session_stats_t){
        .opened = atomic_load(&s_opened),
        .closed = atomic_load(&s_closed),
        .lru_purges = atomic_load(&s_purges),
        .handshake_errors = atomic_load(&s_handshake_errors),
    };
}

void web_session_tls_event(esp_https_server_user_cb_arg_t *event)
{
    int fd = -1;
    esp_tls_get_conn_sockfd(event->tls, &fd);
    session_t *session = slot_for_fd(fd);
    switch (event->user_cb_state) {
    case HTTPD_SSL_USER_CB_SESS_CREATE: {
        const unsigned id = atomic_fetch_add(&s_opened, 1) + 1;
        if (session) {
            *session = (session_t){.id = id, .recv = session->recv};
        }
        ESP_LOGI(TAG, "TLS open id=%u fd=%d", id, fd);
        break;
    }
    case HTTPD_SSL_USER_CB_SESS_ERROR:
        atomic_fetch_add(&s_handshake_errors, 1);
        ESP_LOGI(TAG, "TLS handshake failed fd=%d uri=-", fd);
        break;
    case HTTPD_SSL_USER_CB_SESS_CLOSE:
        atomic_fetch_add(&s_closed, 1);
        if (s_closing_lru) {
            atomic_fetch_add(&s_purges, 1);
        }
        ESP_LOGI(TAG, "TLS close id=%u fd=%d requests=%u first=%s last=%s reason=%s",
                 session ? session->id : 0, fd, session ? session->requests : 0,
                 session && session->first[0] ? session->first : "-",
                 session && session->last[0] ? session->last : "-",
                 s_closing_lru ? "lru-purge" :
                 session && session->close_reason ? session->close_reason : "peer-or-httpd");
        if (session) {
            *session = (session_t){0};
        }
        break;
    }
}

void web_session_request(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    session_t *session = slot_for_fd(fd);
    if (!session || !session->id) {
        return;
    }
    // Record the path only, dropping query strings and non-printable bytes.
    const char *uri = req->uri;
    size_t i = 0;
    for (; i < sizeof(session->last) - 1 && uri[i] >= '!' && uri[i] <= '~' && uri[i] != '?'; ++i) {
        session->last[i] = uri[i];
    }
    session->last[i] = '\0';
    session->close_reason = NULL;
    ++session->requests;
    if (!session->first[0]) {
        memcpy(session->first, session->last, i + 1);
        const char *method = req->method == HTTP_GET ? "GET" : req->method == HTTP_POST ? "POST" : "other";
        ESP_LOGI(TAG, "TLS request id=%u fd=%d method=%s uri=%s", session->id, fd, method, session->first);
    }
}

void web_session_close_reason(httpd_req_t *req, const char *reason)
{
    session_t *session = slot_for_fd(httpd_req_to_sockfd(req));
    if (session && session->id) {
        session->close_reason = reason;
    }
}

esp_err_t web_session_unknown_route(httpd_req_t *req, httpd_err_code_t error)
{
    web_session_request(req);
    web_session_close_reason(req, error == HTTPD_404_NOT_FOUND ? "route-not-found" : "method-not-allowed");
    httpd_resp_send_err(req, error, NULL);
    return ESP_FAIL;   // preserve the SDK default: close after sending the error
}

struct httpd_data;

esp_err_t __real_httpd_sess_close_lru(struct httpd_data *server);
esp_err_t __real_httpd_sess_close_lru_direct(struct httpd_data *server);
esp_err_t __real_httpd_queue_work(httpd_handle_t server, httpd_work_fn_t work, void *arg);

typedef struct {
    httpd_work_fn_t work;
    void *arg;
} lru_work_t;

esp_err_t __real_httpd_sess_set_recv_override(httpd_handle_t server, int fd, httpd_recv_func_t recv);

static int tls_recv(httpd_handle_t server, int fd, char *buf, size_t len, int flags)
{
    session_t *session = slot_for_fd(fd);
    const int received = session->recv(server, fd, buf, len, flags);
    if (!session->close_reason) {
        if (received == 0 || received == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            session->close_reason = "peer-closed";
        } else if (received < 0 && received != MBEDTLS_ERR_SSL_WANT_READ &&
                   received != MBEDTLS_ERR_SSL_WANT_WRITE) {
            session->close_reason = received == MBEDTLS_ERR_SSL_TIMEOUT || received == HTTPD_SOCK_ERR_TIMEOUT
                                    ? "recv-timeout" : "tls-read-error";
        }
    }
    return received;
}

esp_err_t __wrap_httpd_sess_set_recv_override(httpd_handle_t server, int fd, httpd_recv_func_t recv)
{
    session_t *session = slot_for_fd(fd);
    if (!session || !recv) {
        return __real_httpd_sess_set_recv_override(server, fd, recv);
    }
    session->recv = recv;
    return __real_httpd_sess_set_recv_override(server, fd, tls_recv);
}

static void close_lru(void *arg)
{
    const lru_work_t job = *(lru_work_t *)arg;
    free(arg);
    s_closing_lru = true;
    job.work(job.arg);
    s_closing_lru = false;
}

// The SDK's TLS close hook has no reason argument. Wrap only the LRU work
// so a normal close on a full server is never mislabeled as an eviction.
esp_err_t __wrap_httpd_sess_close_lru(struct httpd_data *server)
{
    s_queueing_lru = true;
    const esp_err_t result = __real_httpd_sess_close_lru(server);
    s_queueing_lru = false;
    return result;
}

esp_err_t __wrap_httpd_sess_close_lru_direct(struct httpd_data *server)
{
    s_closing_lru = true;
    const esp_err_t result = __real_httpd_sess_close_lru_direct(server);
    s_closing_lru = false;
    return result;
}

esp_err_t __wrap_httpd_queue_work(httpd_handle_t server, httpd_work_fn_t work, void *arg)
{
    if (!s_queueing_lru) {
        return __real_httpd_queue_work(server, work, arg);
    }
    lru_work_t *job = malloc(sizeof(*job));
    if (!job) {
        ESP_LOGW(TAG, "TLS LRU close could not be traced");
        return __real_httpd_queue_work(server, work, arg);
    }
    *job = (lru_work_t){.work = work, .arg = arg};
    const esp_err_t result = __real_httpd_queue_work(server, close_lru, job);
    if (result != ESP_OK) {
        free(job);
    }
    return result;
}
