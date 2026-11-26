/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "rigctld.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_timer.h"
#include <string.h>
#include <unistd.h>
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_config.h"
#include "rigctl_protocol.h"

#define MAX_CLIENTS 2
#define SEND_TIMEOUT_MS 1000
#define CAT_RESULT_POLL_US 20000
static const char *TAG = "rigctld";
static atomic_bool s_enabled, s_listening, s_debug;
static portMUX_TYPE s_reject_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_last_rejected[RIGCTL_LINE_SIZE];
static atomic_uint s_port, s_clients, s_commands, s_rejected;
static atomic_uint s_opened, s_closed, s_accept_errors, s_send_errors, s_send_timeouts;

typedef struct {
    int fd;
    char input[RIGCTL_LINE_SIZE];
    size_t input_len;
    char output[RIGCTL_REPLY_SIZE];
    size_t output_len, output_sent;
    unsigned ticket;
    int64_t cat_deadline;
    int64_t send_deadline;
    bool closing;
    bool discarding;
    char ip[INET_ADDRSTRLEN];
    char request[RIGCTL_LINE_SIZE];
} client_t;

rigctld_stats_t rigctld_stats(void)
{
    rigctld_stats_t stats = {
        .enabled = atomic_load(&s_enabled), .port = atomic_load(&s_port),
        .listening = atomic_load(&s_listening), .debug = atomic_load(&s_debug),
        .clients = atomic_load(&s_clients), .commands = atomic_load(&s_commands),
        .opened = atomic_load(&s_opened), .closed = atomic_load(&s_closed),
        .accept_errors = atomic_load(&s_accept_errors), .send_errors = atomic_load(&s_send_errors),
        .send_timeouts = atomic_load(&s_send_timeouts),
    };
    taskENTER_CRITICAL(&s_reject_lock);
    stats.rejected = atomic_load(&s_rejected);
    memcpy(stats.last_rejected, s_last_rejected, sizeof(stats.last_rejected));
    taskEXIT_CRITICAL(&s_reject_lock);
    return stats;
}

static bool set_radio(const rigctl_set_t *set, void *context)
{
    client_t *client = context;
    const unsigned ticket = client->ticket;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    switch (set->action) {
    case RIGCTL_FREQ: err = cat_rigctl_set_freq(set->sub, set->value, ticket); break;
    case RIGCTL_MODE: err = cat_rigctl_set_mode(set->sub, (char)set->value, set->width, ticket); break;
    case RIGCTL_VFO: err = cat_rigctl_select_vfo(set->sub, ticket); break;
    case RIGCTL_SPLIT: err = cat_rigctl_set_split(set->sub, ticket); break;
    }
    if (err == ESP_OK) client->cat_deadline = esp_timer_get_time() + 1600000;
    return err == ESP_OK;
}

static void close_socket(int fd, bool discard)
{
    if (discard) {
        // Abort a backed-up TCP socket and release its queued packets instead of waiting for FIN.
        const struct linger reset = {.l_onoff = 1, .l_linger = 0};
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    }
    close(fd);
}

static void close_client(client_t *client, bool discard)
{
    close_socket(client->fd, discard);
    *client = (client_t){.fd = -1};
    atomic_fetch_sub(&s_clients, 1);
    atomic_fetch_add(&s_closed, 1);
}

static bool socket_retry(void)
{
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
}

static bool configure_client(int fd)
{
    const int one = 1;
    const int keep_idle = 15, keep_interval = 5, keep_count = 3;
    // lwIP also uses SO_SNDTIMEO to bound retries for a graceful close under low memory.
    const struct timeval send_timeout = {SEND_TIMEOUT_MS / 1000, (SEND_TIMEOUT_MS % 1000) * 1000};
    return fcntl(fd, F_SETFL, O_NONBLOCK) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) == 0 &&
           setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0 &&
           setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keep_idle, sizeof(keep_idle)) == 0 &&
           setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keep_interval, sizeof(keep_interval)) == 0 &&
           setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keep_count, sizeof(keep_count)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) == 0;
}

static void accept_client(int listener, client_t clients[MAX_CLIENTS], bool restricted, uint32_t allowed)
{
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    int fd = accept(listener, (struct sockaddr *)&peer, &len);
    if (fd < 0) {
        if (!socket_retry()) atomic_fetch_add(&s_accept_errors, 1);
        return;
    }
    if ((!restricted || peer.sin_addr.s_addr == allowed) && fd < FD_SETSIZE) {
        for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i].fd >= 0) continue;
            if (!configure_client(fd)) {
                atomic_fetch_add(&s_accept_errors, 1);
                break;
            }
            clients[i].fd = fd;
            inet_ntop(AF_INET, &peer.sin_addr, clients[i].ip, sizeof(clients[i].ip));
            atomic_fetch_add(&s_clients, 1);
            atomic_fetch_add(&s_opened, 1);
            return;
        }
    }
    close_socket(fd, true);
}

static void printable(char *out, const char *in, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = (unsigned char)in[i];
        out[i] = ch == '\n' ? '|' : (ch >= 0x20 && ch <= 0x7e ? (char)ch : '.');
    }
    out[length] = '\0';
}

static void report_reply(const client_t *client, bool rejected)
{
    if (rejected) {
        taskENTER_CRITICAL(&s_reject_lock);
        atomic_fetch_add(&s_rejected, 1);
        memcpy(s_last_rejected, client->request, sizeof(s_last_rejected));
        taskEXIT_CRITICAL(&s_reject_lock);
    }
    if (rejected || atomic_load(&s_debug)) {
        char response[RIGCTL_REPLY_SIZE];
        printable(response, client->output, client->output_len);
        ESP_LOGI(TAG, "%s %s RX [%s] -> [%s]", client->ip, rejected ? "rejected" : "command",
                 client->request, response);
    }
}

static void consume(client_t *client, size_t count)
{
    client->input_len -= count;
    memmove(client->input, client->input + count, client->input_len);
}

static rigctl_reply_t reject_line(client_t *client)
{
    strcpy(client->output, "RPRT -11\n");
    return (rigctl_reply_t){.length = sizeof("RPRT -11\n") - 1, .rejected = true};
}

static void process_line(client_t *client, unsigned ticket)
{
    char *lf = memchr(client->input, '\n', client->input_len);
    if (client->discarding) {
        consume(client, lf ? (size_t)(lf - client->input) + 1 : client->input_len);
        client->discarding = lf == NULL;
        return;
    }

    const size_t len = lf ? (size_t)(lf - client->input) : client->input_len;
    if (!lf && len < sizeof(client->input)) return;
    size_t log_len = len < sizeof(client->request) ? len : sizeof(client->request) - 1;
    if (log_len && client->input[log_len - 1] == '\r') --log_len;
    printable(client->request, client->input, log_len);

    rigctl_reply_t reply;
    if (!lf || memchr(client->input, '\0', len)) {
        client->discarding = lf == NULL;
        reply = reject_line(client);
    } else {
        *lf = '\0';
        cat_state_t cat;
        cat_get_state(&cat);
        client->ticket = ticket;
        reply = rigctl_command(client->input, &cat, set_radio, client, client->output);
    }
    consume(client, len + (lf != NULL));
    if (reply.length || reply.close) atomic_fetch_add(&s_commands, 1);
    client->closing = reply.close;
    client->output_len = reply.length;
    client->output_sent = 0;
    if (!client->cat_deadline) report_reply(client, reply.rejected);
}

static void expire_send(client_t *client)
{
    atomic_fetch_add(&s_send_timeouts, 1);
    ESP_LOGI(TAG, "%s reply timed out after %u ms (%u/%u bytes queued)",
             client->ip, SEND_TIMEOUT_MS, (unsigned)client->output_sent, (unsigned)client->output_len);
    close_client(client, true);
}

static void send_reply(client_t *client)
{
    if (esp_timer_get_time() >= client->send_deadline) {
        expire_send(client);
        return;
    }
    const ssize_t n = send(client->fd, client->output + client->output_sent,
                           client->output_len - client->output_sent, MSG_DONTWAIT);
    if (n > 0) {
        client->output_sent += (size_t)n;
        if (client->output_sent == client->output_len) client->send_deadline = 0;
    } else if (!n || !socket_retry()) {
        atomic_fetch_add(&s_send_errors, 1);
        close_client(client, true);
    }
}

static void serve(int listener, bool restricted, uint32_t allowed)
{
    client_t clients[MAX_CLIENTS] = {{.fd = -1}, {.fd = -1}};
    unsigned sequence[MAX_CLIENTS] = {0};
    while (true) {
        int max_fd = listener;
        fd_set reads, writes;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_SET(listener, &reads);
        bool buffered = false;
        int64_t next_wake = INT64_MAX;
        for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
            client_t *c = &clients[i];
            if (c->fd < 0) continue;
            if (c->closing && c->output_sent == c->output_len) {
                close_client(c, false);
                continue;
            }
            if (c->output_sent == c->output_len && c->input_len) {
                sequence[i] = (sequence[i] % 0x1fffffffu) + 1u;
                process_line(c, (sequence[i] << 1) | i);
            }
            if (c->cat_deadline) {
                const int result = cat_rigctl_result(c->ticket);
                const int64_t now = esp_timer_get_time();
                if (!result && now < c->cat_deadline) {
                    if (now + CAT_RESULT_POLL_US < next_wake) next_wake = now + CAT_RESULT_POLL_US;
                    continue;
                }
                if (result < 1) {
                    // Preserve the extended-response header if the client requested it.
                    const size_t head = c->output_len - (sizeof("RPRT 0\n") - 1);
                    c->output_len = head + (size_t)snprintf(c->output + head,
                                        sizeof(c->output) - head, "RPRT -11\n");
                }
                c->cat_deadline = 0;
                report_reply(c, result < 1);
            }
            if (c->fd < 0) continue;
            if (c->output_sent < c->output_len) {
                const int64_t now = esp_timer_get_time();
                if (!c->send_deadline) c->send_deadline = now + SEND_TIMEOUT_MS * 1000;
                if (now >= c->send_deadline) {
                    expire_send(c);
                    continue;
                }
                if (c->send_deadline < next_wake) next_wake = c->send_deadline;
                FD_SET(c->fd, &writes);
            } else {
                FD_SET(c->fd, &reads);
                buffered |= memchr(c->input, '\n', c->input_len) != NULL;
            }
            if (c->fd > max_fd) max_fd = c->fd;
        }
        struct timeval timeout = {0};
        if (!buffered && next_wake != INT64_MAX) {
            const int64_t wait_us = next_wake - esp_timer_get_time();
            if (wait_us > 0) {
                timeout.tv_sec = wait_us / 1000000;
                timeout.tv_usec = wait_us % 1000000;
            }
        }
        const int ready = select(max_fd + 1, &reads, &writes, NULL,
                                 buffered || next_wake != INT64_MAX ? &timeout : NULL);
        if (ready < 0 && !socket_retry()) break;
        if (ready > 0) {
            for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
                client_t *c = &clients[i];
                if (c->fd < 0) continue;
                if (FD_ISSET(c->fd, &reads)) {
                    const ssize_t n = recv(c->fd, c->input + c->input_len,
                                           sizeof(c->input) - c->input_len, MSG_DONTWAIT);
                    if (n > 0) c->input_len += (size_t)n;
                    else if (!n || !socket_retry()) close_client(c, n < 0);
                } else if (FD_ISSET(c->fd, &writes)) {
                    send_reply(c);
                }
            }
            if (FD_ISSET(listener, &reads)) accept_client(listener, clients, restricted, allowed);
        }
        // A continuously readable socket must still leave time for idle and other work.
        vTaskDelay(1);
    }
    for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd >= 0) close_client(&clients[i], true);
    }
}

static void rigctld_task(void *arg)
{
    (void)arg;
    app_config_t cfg;
    app_config_get(&cfg);
    struct in_addr allowed = {0};
    const bool restricted = cfg.rigctl_ip[0] != '\0';
    if (restricted && inet_pton(AF_INET, cfg.rigctl_ip, &allowed) != 1) {
        ESP_LOGE(TAG, "Invalid configured IPv4 address. NET rigctl did not start");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(cfg.rigctl_port), .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener >= 0 && listener < FD_SETSIZE && fcntl(listener, F_SETFL, O_NONBLOCK) == 0 &&
        bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0 &&
        listen(listener, MAX_CLIENTS) == 0) {
        atomic_store(&s_listening, true);
        ESP_LOGI(TAG, "NET rigctl listening on TCP %u, allowed IPv4: %s",
                 cfg.rigctl_port, restricted ? cfg.rigctl_ip : "any");
        serve(listener, restricted, allowed.s_addr);
        atomic_store(&s_listening, false);
        ESP_LOGE(TAG, "NET rigctl listener stopped");
    } else {
        ESP_LOGE(TAG, "Could not listen on TCP %u (errno %d)", cfg.rigctl_port, errno);
    }
    if (listener >= 0) close(listener);
    vTaskDelete(NULL);
}

void rigctld_start(void)
{
    app_config_t cfg;
    app_config_get(&cfg);
    atomic_store(&s_debug, cfg.rigctl_debug);
    atomic_store(&s_port, cfg.rigctl_port);
    atomic_store(&s_enabled, cfg.rigctl_enabled);
    if (!cfg.rigctl_enabled) {
        ESP_LOGI(TAG, "NET rigctl disabled");
        return;
    }
    if (xTaskCreatePinnedToCore(rigctld_task, "rigctld", 6144, NULL, 1, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Could not start NET rigctl task");
    }
}
