/* SPDX-License-Identifier: MIT */
#include "yaesu_catd.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "app_config.h"
#include "cat.h"

#define MAX_CLIENTS        2
#define INPUT_SIZE         128
#define OUTPUT_SIZE        512
#define SEND_TIMEOUT_US    1000000LL
#define FREQ_FLUSH_US      100000LL   // send at most one frequency change per VFO every 100 ms
#define FREQ_HOLD_US       1500000LL  // report a requested frequency until the radio confirms it
#define POLL_US            50000LL    // auto information and pending frequency checks

static const char *TAG = "yaesu_catd";
static atomic_bool s_enabled, s_listening, s_debug;
static atomic_uint s_port, s_clients, s_commands, s_rejected, s_writes;
static atomic_uint s_opened, s_closed, s_accept_errors, s_send_errors;
static portMUX_TYPE s_reject_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_last_rejected[YAESU_CAT_CMD_MAX + 1];

typedef struct {
    int fd;
    char input[INPUT_SIZE];
    size_t input_len;
    char output[OUTPUT_SIZE];
    size_t output_len, output_sent;
    int64_t send_deadline;
    bool discarding;
    yaesu_session_t session;
    cat_state_t reported;   // last state pushed with auto information
    char ip[INET_ADDRSTRLEN];
} client_t;

/* Frequency writes are coalesced: a program that follows a tuning knob sends many. */
typedef struct {
    bool pending;
    uint32_t hz;
    int64_t requested_at;
    int64_t sent_at;
    uint32_t held_hz;
    int64_t held_until;
} freq_slot_t;

static freq_slot_t s_freq[2];

yaesu_catd_stats_t yaesu_catd_stats(void)
{
    yaesu_catd_stats_t stats = {
        .enabled = atomic_load(&s_enabled), .listening = atomic_load(&s_listening),
        .debug = atomic_load(&s_debug), .port = atomic_load(&s_port),
        .clients = atomic_load(&s_clients), .commands = atomic_load(&s_commands),
        .writes = atomic_load(&s_writes), .opened = atomic_load(&s_opened),
        .closed = atomic_load(&s_closed), .accept_errors = atomic_load(&s_accept_errors),
        .send_errors = atomic_load(&s_send_errors),
    };
    taskENTER_CRITICAL(&s_reject_lock);
    stats.rejected = atomic_load(&s_rejected);
    memcpy(stats.last_rejected, s_last_rejected, sizeof(stats.last_rejected));
    taskEXIT_CRITICAL(&s_reject_lock);
    return stats;
}

/* The radio state as the network client should see it: requested frequencies first. */
static void current_state(cat_state_t *cat)
{
    cat_get_state(cat);
    const int64_t now = esp_timer_get_time();
    for (unsigned sub = 0; sub < 2; ++sub) {
        freq_slot_t *f = &s_freq[sub];
        uint32_t *hz = sub ? &cat->freq_sub : &cat->freq_main;
        if (f->pending) {
            *hz = f->hz;
        } else if (f->held_until > now && *hz != f->held_hz) {
            *hz = f->held_hz;
        } else {
            f->held_until = 0;
        }
    }
}

static void flush_frequencies(void)
{
    const int64_t now = esp_timer_get_time();
    for (unsigned sub = 0; sub < 2; ++sub) {
        freq_slot_t *f = &s_freq[sub];
        if (!f->pending || now - f->sent_at < FREQ_FLUSH_US) continue;
        const esp_err_t err = cat_set_freq(sub, f->hz);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Frequency %lu Hz not applied: %s", (unsigned long)f->hz, esp_err_to_name(err));
        }
        if (err == ESP_ERR_TIMEOUT) continue;   // the CAT queue is full: retry with the latest value
        f->pending = false;
        f->sent_at = now;
        f->held_hz = f->hz;
        f->held_until = err == ESP_OK ? now + FREQ_HOLD_US : 0;
    }
}

static bool apply(const yaesu_set_t *set, void *context)
{
    (void)context;
    esp_err_t err = ESP_OK;
    switch (set->action) {
    case YAESU_SET_FREQ: {
        freq_slot_t *f = &s_freq[set->sub];
        if (!f->pending) f->requested_at = esp_timer_get_time();
        f->pending = true;
        f->hz = set->value;
        flush_frequencies();
        return true;
    }
    case YAESU_SET_MODE: err = cat_set_mode_char(set->sub, (char)set->value); break;
    case YAESU_SELECT_VFO: err = cat_rigctl_select_vfo(set->sub, 0); break;
    case YAESU_SET_TX_VFO: {
        cat_state_t cat;
        cat_get_state(&cat);
        err = cat_rigctl_set_split(cat.rx_vfo_sub, set->sub, false, 0);
        break;
    }
    case YAESU_SWAP_VFO: err = cat_swap_vfo(); break;
    case YAESU_COPY_A_TO_B: err = cat_copy_main_to_sub(); break;
    case YAESU_COPY_B_TO_A: err = cat_copy_sub_to_main(); break;
    }
    return err == ESP_OK;
}

static void printable(char *out, const char *in, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = (unsigned char)in[i];
        out[i] = ch >= 0x20 && ch <= 0x7e ? (char)ch : '.';
    }
    out[length] = '\0';
}

static void queue_output(client_t *client, const char *data, size_t length)
{
    if (!length) return;
    if (client->output_len + length > sizeof(client->output)) {
        // A client that does not read its replies is closed by the send timeout.
        return;
    }
    memcpy(client->output + client->output_len, data, length);
    client->output_len += length;
    if (!client->send_deadline) client->send_deadline = esp_timer_get_time() + SEND_TIMEOUT_US;
}

static void handle_command(client_t *client, const char *cmd)
{
    cat_state_t cat;
    current_state(&cat);
    char reply[YAESU_CAT_REPLY_MAX];
    const yaesu_reply_t r = yaesu_cat_command(cmd, &cat, &client->session, apply, client, reply);
    atomic_fetch_add(&s_commands, 1);
    if (r.write) atomic_fetch_add(&s_writes, 1);
    if (r.rejected) {
        taskENTER_CRITICAL(&s_reject_lock);
        atomic_fetch_add(&s_rejected, 1);
        strlcpy(s_last_rejected, cmd, sizeof(s_last_rejected));
        taskEXIT_CRITICAL(&s_reject_lock);
    }
    if (r.rejected || atomic_load(&s_debug)) {
        ESP_LOGI(TAG, "%s %s [%s;] -> [%s]", client->ip, r.rejected ? "rejected" : "command", cmd,
                 r.length ? reply : "");
    }
    queue_output(client, reply, r.length);
    // Auto information starts from the state this client has just been told.
    if (!strncmp(cmd, "AI", 2) || !strncmp(cmd, "ai", 2)) current_state(&client->reported);
}

static void process_input(client_t *client)
{
    size_t start = 0;
    for (size_t i = 0; i < client->input_len; ++i) {
        const char ch = client->input[i];
        if (ch != ';') continue;
        char cmd[YAESU_CAT_CMD_MAX + 1];
        size_t n = 0;
        for (size_t j = start; j < i; ++j) {
            // Line breaks or spaces typed in a terminal are ignored, as is anything non-printable.
            const unsigned char c = (unsigned char)client->input[j];
            if (c <= ' ' || c > '~') continue;
            if (n < YAESU_CAT_CMD_MAX) cmd[n++] = (char)c;
            else n = YAESU_CAT_CMD_MAX + 1;
            if (n > YAESU_CAT_CMD_MAX) break;
        }
        start = i + 1;
        if (client->discarding) {
            client->discarding = false;
            continue;
        }
        if (n == 0) continue;
        if (n > YAESU_CAT_CMD_MAX) {
            queue_output(client, "?;", 2);
            continue;
        }
        cmd[n] = '\0';
        handle_command(client, cmd);
    }
    client->input_len -= start;
    memmove(client->input, client->input + start, client->input_len);
    if (client->input_len == sizeof(client->input)) {
        // No ';' in a full buffer: drop it and the rest of that command.
        char shown[33];
        printable(shown, client->input, 32);
        ESP_LOGW(TAG, "%s command too long, dropped: %s...", client->ip, shown);
        client->input_len = 0;
        client->discarding = true;
    }
}

static void push_changes(client_t *client)
{
    if (!client->session.ai) return;
    cat_state_t now;
    current_state(&now);
    char changes[96];
    const size_t n = yaesu_cat_changes(&client->reported, &now, changes, sizeof(changes));
    client->reported = now;
    queue_output(client, changes, n);
}

static void close_client(client_t *client, bool discard)
{
    if (discard) {
        const struct linger reset = {.l_onoff = 1, .l_linger = 0};
        setsockopt(client->fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    }
    close(client->fd);
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
    return fcntl(fd, F_SETFL, O_NONBLOCK) == 0 &&
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
    const int fd = accept(listener, (struct sockaddr *)&peer, &len);
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
            clients[i] = (client_t){.fd = fd};
            inet_ntop(AF_INET, &peer.sin_addr, clients[i].ip, sizeof(clients[i].ip));
            current_state(&clients[i].reported);
            atomic_fetch_add(&s_clients, 1);
            atomic_fetch_add(&s_opened, 1);
            ESP_LOGI(TAG, "Client %s connected", clients[i].ip);
            return;
        }
    }
    const struct linger reset = {.l_onoff = 1, .l_linger = 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    close(fd);
}

static void send_output(client_t *client)
{
    const ssize_t n = send(client->fd, client->output + client->output_sent,
                           client->output_len - client->output_sent, MSG_DONTWAIT);
    if (n > 0) {
        client->output_sent += (size_t)n;
        if (client->output_sent == client->output_len) {
            client->output_len = client->output_sent = 0;
            client->send_deadline = 0;
        }
    } else if (!n || !socket_retry()) {
        atomic_fetch_add(&s_send_errors, 1);
        close_client(client, true);
    }
}

static void serve(int listener, bool restricted, uint32_t allowed)
{
    client_t clients[MAX_CLIENTS] = {{.fd = -1}, {.fd = -1}};
    while (true) {
        flush_frequencies();
        int max_fd = listener;
        fd_set reads, writes;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_SET(listener, &reads);
        const int64_t now = esp_timer_get_time();
        for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
            client_t *c = &clients[i];
            if (c->fd < 0) continue;
            push_changes(c);
            if (c->output_len > c->output_sent) {
                if (now >= c->send_deadline) {
                    atomic_fetch_add(&s_send_errors, 1);
                    ESP_LOGI(TAG, "%s did not read its replies, closing", c->ip);
                    close_client(c, true);
                    continue;
                }
                FD_SET(c->fd, &writes);
            }
            FD_SET(c->fd, &reads);
            if (c->fd > max_fd) max_fd = c->fd;
        }
        struct timeval timeout = {.tv_sec = 0, .tv_usec = POLL_US};
        const int ready = select(max_fd + 1, &reads, &writes, NULL, &timeout);
        if (ready < 0 && !socket_retry()) break;
        if (ready > 0) {
            for (unsigned i = 0; i < MAX_CLIENTS; ++i) {
                client_t *c = &clients[i];
                if (c->fd < 0) continue;
                if (FD_ISSET(c->fd, &reads)) {
                    const ssize_t n = recv(c->fd, c->input + c->input_len,
                                           sizeof(c->input) - c->input_len, MSG_DONTWAIT);
                    if (n > 0) {
                        c->input_len += (size_t)n;
                        process_input(c);
                    } else if (!n || !socket_retry()) {
                        ESP_LOGI(TAG, "Client %s disconnected", c->ip);
                        close_client(c, n < 0);
                        continue;
                    }
                }
                if (c->fd >= 0 && FD_ISSET(c->fd, &writes)) send_output(c);
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

static void yaesu_catd_task(void *arg)
{
    (void)arg;
    app_config_t cfg;
    app_config_get(&cfg);
    struct in_addr allowed = {0};
    const bool restricted = cfg.rigctl_ip[0] != '\0';
    if (restricted && inet_pton(AF_INET, cfg.rigctl_ip, &allowed) != 1) {
        ESP_LOGE(TAG, "Invalid configured IPv4 address. Yaesu CAT server did not start");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(cfg.yaesu_port), .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener >= 0 && listener < FD_SETSIZE && fcntl(listener, F_SETFL, O_NONBLOCK) == 0 &&
        bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0 &&
        listen(listener, MAX_CLIENTS) == 0) {
        atomic_store(&s_listening, true);
        ESP_LOGI(TAG, "Yaesu CAT server listening on TCP %u, allowed IPv4: %s",
                 cfg.yaesu_port, restricted ? cfg.rigctl_ip : "any");
        serve(listener, restricted, allowed.s_addr);
        atomic_store(&s_listening, false);
        ESP_LOGE(TAG, "Yaesu CAT server stopped");
    } else {
        ESP_LOGE(TAG, "Could not listen on TCP %u (errno %d)", cfg.yaesu_port, errno);
    }
    if (listener >= 0) close(listener);
    vTaskDelete(NULL);
}

void yaesu_catd_start(void)
{
    app_config_t cfg;
    app_config_get(&cfg);
    atomic_store(&s_debug, cfg.yaesu_debug);
    atomic_store(&s_port, cfg.yaesu_port);
    atomic_store(&s_enabled, cfg.yaesu_enabled);
    if (!cfg.yaesu_enabled) {
        ESP_LOGI(TAG, "Yaesu CAT server disabled");
        return;
    }
    if (xTaskCreatePinnedToCore(yaesu_catd_task, "yaesu_catd", 6144, NULL, 1, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Could not start the Yaesu CAT server task");
    }
}
