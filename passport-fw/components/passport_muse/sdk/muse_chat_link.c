/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Muse chat turns on boards without PSRAM (CONFIG_MUSE_HATCH=n). There's no room
 * for a second TLS and Noise connection or for MP3 decoding, so turns ride
 * Home Link's own session (muse_link_req_*) and replies are text only:
 * A live POST /chat/subscribe receives NDJSON replies while POST /chat/stream
 * uploads the note. Both use the existing device token and Link connection.
 * Lines are parsed without cJSON; bounded buffers keep internal RAM use small.
 */
#include "muse_chat.h"
#include "muse_chat_priv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_link.h"
#include "muse_wifi.h"

static const char *TAG = "muse_chat_link";

#define MIC_RATE 16000
#define STAGE_BYTES 768                 /* 1 KB of base64, 24 ms; leaves room for Wi-Fi/TLS */
#define CHUNK_BYTES (STAGE_BYTES / 3 * 4)
#define ACK_MAX 2048
#define ROW_MAX 12288                   /* former RAM limit; large-event diagnostic threshold */
#define ROW_STREAM_MAX (4U * 1024U * 1024U) /* work bound, not a RAM allocation */
#define TEXT_MAX 1024                   /* reply text kept for the captions */
#define EV_TEXT 72
#define SEND_WAIT_MS 200               /* the press queues the pre-roll all at once */
#define REPLY_TIMEOUT_US 60000000
#define SYNC_RETRY_US 2000000
#define SYNC_TIMEOUT_US 60000000
#define CHAIN_MAX 4                    /* recent assistant parents, plus the ACK IDs */


/* A reply being received on Link's session task, read by the voice task once done. */
typedef struct {
    uint32_t gen;       /* bumped per request; frames of older ones are dropped */
    int status;         /* HTTP status, -1 if the request died */
    bool done;
    bool overflow;
    size_t len;
    size_t cap;         /* fixed note ACK buffer; 0: incremental subscription parser */
    char *body;
} rx_t;

/* A subscription event, decoded without allocating a JSON tree. */
typedef struct {
    char type[16];
    bool ready;         /* display_text_ready */
    uint64_t seq;
    char event[24];
    char msg[80];
    char reply_to[80];
    char text[TEXT_MAX];
} row_t;

/* One lexical token plus the projected payload; never retain raw NDJSON.
 * Unknown nested values are validated and discarded as bytes arrive. */
typedef struct {
    row_t payload;
    char token[TEXT_MAX];
    struct { uint8_t kind, state, scope, field; } stack[32];
    size_t used, cap;
    uint32_t unicode, high;
    uint16_t payload_fields;
    uint8_t depth, mode, escape, digits, rank[2];
    bool id[2], parent[2], key, clipped, bad, complete;
} row_stream_t;

enum { RX_NOTE, RX_SUB, RX_COUNT };

typedef enum { T_IDLE, T_TALKING, T_ACK, T_REPLY } phase_t;

typedef struct {
    char text[EV_TEXT];
    muse_hatch_ev_t type;
} ev_t;

static SemaphoreHandle_t s_rx_lock;
static StaticSemaphore_t s_rx_lock_storage;
static rx_t s_rx[RX_COUNT];
static int64_t s_stream[RX_COUNT];      /* open request per slot, 0 none */
static QueueHandle_t s_events;
static StaticQueue_t s_event_storage;
static uint8_t s_event_items[8 * sizeof(ev_t)];
/* Subscription callback state, always under s_rx_lock. Keep the two newest
 * completed messages until the voice task drains them, including before ACK. */
static unsigned s_pending_count;
static uint64_t s_last_seq;
static bool s_early_evicted, s_skipped_big, s_receive_oom;
static char s_note_id[80], s_parent_id[80]; /* ACK IDs shared under s_rx_lock */
static char s_chain[CHAIN_MAX][80];
static unsigned s_chain_count;

/* Voice task only. */
typedef struct {
    phase_t phase;
    uint8_t *stage;                     /* PCM waiting for the next chunk */
    size_t stage_len;
    char *chunk;
    char note_id[80], parent_id[80], seen[2][80];
    unsigned seen_count;
    uint32_t seen_hash[2];
    int64_t t_end, t_reply, t_retry, t_disconnected;
    bool reconnecting;
    bool heard, replied;
    bool after_note;
    char error[EV_TEXT];                /* why the turn failed, repeated at the release */
    char text[TEXT_MAX];
} turn_t;

/* These bounded parsing buffers used to consume about 9 KB of .bss for every
 * app. The launcher has one active app, so allocate them only for a Muse
 * session and return them before Radio or Xiaozhi starts. */
typedef struct {
    row_t *row, *delta, *pending, *pump_pending;
    char *ack;
    row_stream_t *stream;
} chat_mem_t;

static void chat_mem_free(chat_mem_t *mem)
{
    if (!mem) return;
    free(mem->row); free(mem->delta); free(mem->pending);
    free(mem->pump_pending); free(mem->ack); free(mem->stream); free(mem);
}

static chat_mem_t *chat_mem_new(void)
{
    chat_mem_t *mem = calloc(1, sizeof(*mem));
    if (!mem) return NULL;
    mem->row = calloc(1, sizeof(row_t));
    mem->delta = calloc(1, sizeof(row_t));
    mem->pending = calloc(2, sizeof(row_t));
    mem->pump_pending = calloc(1, sizeof(row_t));
    mem->ack = calloc(1, ACK_MAX);
    mem->stream = calloc(1, sizeof(row_stream_t));
    if (!mem->row || !mem->delta || !mem->pending || !mem->pump_pending || !mem->ack || !mem->stream) {
        chat_mem_free(mem);
        return NULL;
    }
    return mem;
}

static chat_mem_t *s_mem;
static turn_t *s_turn_mem;
#define s_row (*s_mem->row)
#define s_delta (*s_mem->delta)
#define s_pending (s_mem->pending)
#define s_turn (*s_turn_mem)
#define s_pump_pending (*s_mem->pump_pending)

static bool related(const row_t *row)
{
    if (!strcmp(row->event, "message.user")) {
        return !s_note_id[0] || !strcmp(row->msg, s_note_id) || !strcmp(row->msg, s_parent_id);
    }
    if (!s_note_id[0] || !row->reply_to[0]
        || !strcmp(row->reply_to, s_note_id) || !strcmp(row->reply_to, s_parent_id)) return true;
    for (unsigned i = 0; i < s_chain_count; i++)
        if (!strcmp(row->reply_to, s_chain[i])) return true;
    return false;
}

/* Called only under the receive lock. Follow replies to replies without accepting
 * an explicit parent from an unrelated conversation. Keep RAM bounded. */
static void remember_parent(const char *id)
{
    for (unsigned i = 0; i < s_chain_count; i++)
        if (!strcmp(s_chain[i], id)) return;
    if (s_chain_count == CHAIN_MAX) {
        memmove(s_chain[0], s_chain[1], (CHAIN_MAX - 1) * sizeof(s_chain[0]));
        s_chain_count--;
    }
    strlcpy(s_chain[s_chain_count++], id, sizeof(s_chain[0]));
}

/* Append whole UTF-8 characters even when the caption is already nearly full. */
static void text_append(char *out, size_t cap, const char *text)
{
    size_t used = strlen(out), n = strlen(text);
    if (n >= cap - used) {
        n = cap - used - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
    }
    memcpy(out + used, text, n);
    out[used + n] = 0;
}

/* Called with the receive lock held; never waits on the voice task. */
static void subscription_event(void)
{
    if (s_row.seq && s_row.seq <= s_last_seq) return;
    if (s_row.seq) s_last_seq = s_row.seq;
    bool append = !strcmp(s_row.event, "delta.text_append");
    bool start = !strcmp(s_row.event, "delta.message_start");
    bool done = !strcmp(s_row.event, "delta.message_done");
    bool full = !strcmp(s_row.event, "message.assistant");
    bool user = !strcmp(s_row.event, "message.user");
    if ((!append && !start && !done && !full && !user) || !s_row.msg[0]) return;
    if (!related(&s_row)) return;
    if (!user) remember_parent(s_row.msg);
    if (start || append) {
        if (strcmp(s_delta.msg, s_row.msg)) {
            memset(&s_delta, 0, sizeof(s_delta));
            strlcpy(s_delta.msg, s_row.msg, sizeof(s_delta.msg));
        }
        if (s_row.reply_to[0]) strlcpy(s_delta.reply_to, s_row.reply_to, sizeof(s_delta.reply_to));
        if (append) {
            text_append(s_delta.text, sizeof(s_delta.text), s_row.text);
        }
        return;
    }
    if (!done && !s_row.ready) return;
    if (!strcmp(s_row.msg, s_delta.msg)) {
        if (!s_row.text[0]) strlcpy(s_row.text, s_delta.text, sizeof(s_row.text));
        if (!s_row.reply_to[0]) strlcpy(s_row.reply_to, s_delta.reply_to, sizeof(s_row.reply_to));
    }
    if (!related(&s_row) || !s_row.text[0]) return;
    if (!user) strlcpy(s_row.event, "message.assistant", sizeof(s_row.event));
    s_row.ready = true;
    /* Coalesce the done event and persisted full message, including before ACK. */
    for (unsigned i = 0; i < s_pending_count; i++) {
        if (!strcmp(s_pending[i].msg, s_row.msg)) { s_pending[i] = s_row; return; }
    }
    /* Keep the two newest finals if the voice task has not drained the queue.
     * Before ACK they are provisional: unrelated traffic must not fail a turn
     * or grow memory without bound. Remember eviction for a useful timeout. */
    if (s_pending_count == 2) {
        if (!s_note_id[0]) s_early_evicted = true;
        s_pending[0] = s_pending[1];
        s_pending_count--;
    }
    s_pending[s_pending_count++] = s_row;
}

#include "muse_row_stream.inc"

static void row_clear(rx_t *rx)
{
    rx->len = 0;
    if (s_mem) stream_reset();
}

static void row_data(rx_t *rx, const uint8_t *data, size_t len, bool end)
{
    if (rx->status != 200) return;
    while (len--) {
        unsigned char c=*data++;
        if (c=='\n') {
            if (!rx->overflow && rx->len) {
                stream_finish();
                if (rx->len>ROW_MAX)
                    ESP_LOGI(TAG,"Subscription event streamed bytes=%u valid=%d event=%s",
                        (unsigned)rx->len,!s_mem->stream->bad && s_mem->stream->complete,s_row.event);
            }
            row_clear(rx); rx->overflow=false;
            continue;
        }
        if (rx->overflow) continue;
        if (!rx->len) stream_reset();
        if (++rx->len>ROW_STREAM_MAX) {
            rx->overflow=s_skipped_big=true;
            ESP_LOGW(TAG,"Subscription row exceeds streaming work limit");
            row_clear(rx); continue;
        }
        stream_byte(s_mem->stream,c);
    }
    if (end) {
        if (rx->len && !rx->overflow) stream_finish();
        row_clear(rx);
    }
}

static void rx_clear(rx_t *rx)
{
    rx->status = 0;
    rx->done = rx->overflow = false;
    rx->len = 0;
    if (!rx->cap) {
        row_clear(rx);
        rx->body = NULL;
        s_skipped_big = false;
        s_receive_oom = false;
        s_pending_count = 0;
        s_last_seq = 0;
        s_early_evicted = false;
        s_note_id[0] = s_parent_id[0] = 0;
        s_chain_count = 0;
        if (s_mem) memset(&s_delta, 0, sizeof(s_delta));
    }
}

static void on_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    uintptr_t token = (uintptr_t)ctx;
    rx_t *rx = &s_rx[token & 1];
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    if (s_mem && (uint32_t)(token >> 1) == rx->gen && !rx->done) {
        if (status) {
            rx->status = status;
        }
        if (!rx->cap) {
            row_data(rx, data, len, end && status >= 0);
        } else if (len && rx->len + len >= rx->cap) {
            rx->overflow = true;
        } else if (len) {
            memcpy(rx->body + rx->len, data, len);
            rx->len += len;
            rx->body[rx->len] = '\0';
        }
        rx->done = end || status < 0;
    }
    xSemaphoreGive(s_rx_lock);
}

/* Starts a request whose reply lands in slot `slot`. False if Link's session is down. */
static bool request(int slot, const char *verb, const char *path, bool json, bool end_body)
{
    char req_id[40];
    snprintf(req_id, sizeof(req_id), "muse-%08" PRIx32 "-%08" PRIx32, esp_random(), esp_random());
    const char *headers[] = { "x-request-id", req_id, "x-app-id", "hatch-web",
                              json ? "Content-Type" : NULL, "application/json",
                              "Accept", slot == RX_SUB ? "application/x-ndjson" : "application/json", NULL };
    rx_t *rx = &s_rx[slot];
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    rx->gen++;
    if (slot == RX_SUB && s_turn.phase == T_REPLY) {
        row_clear(rx); rx->body = NULL; rx->len = 0;
        rx->status = 0; rx->done = rx->overflow = false;
        s_last_seq = 0;
        memset(&s_delta, 0, sizeof(s_delta));
    } else rx_clear(rx);
    if (rx->cap) {
        rx->body[0] = '\0';
    }
    uintptr_t token = (uintptr_t)rx->gen << 1 | slot;
    xSemaphoreGive(s_rx_lock);
    s_stream[slot] = muse_link_req_open(verb, path, headers, end_body, on_frame, (void *)token);
    return s_stream[slot] != 0;
}

/* Drops slot `slot`'s request, if any, and whatever of its reply is still coming. */
static void drop(int slot)
{
    if (s_stream[slot]) {
        muse_link_req_cancel(s_stream[slot]);
        s_stream[slot] = 0;
    }
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    s_rx[slot].gen++;
    rx_clear(&s_rx[slot]);
    xSemaphoreGive(s_rx_lock);
}

/* True once slot `slot`'s reply is complete (or the request died). */
static bool received(int slot)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    bool done = s_stream[slot] && s_rx[slot].done;
    xSemaphoreGive(s_rx_lock);
    if (done) {
        s_stream[slot] = 0;
    }
    return done;
}

static void emit(muse_hatch_ev_t type, const char *text)
{
    ev_t ev = { .type = type };
    muse_hatch_tail_words(text ? text : "", ev.text, sizeof(ev.text));
    xQueueSend(s_events, &ev, 0);
}

static void end_turn(void)
{
    drop(RX_NOTE);
    drop(RX_SUB);
    free(s_turn.stage);
    free(s_turn.chunk);
    s_turn.stage = NULL;
    s_turn.chunk = NULL;
    s_turn.phase = T_IDLE;
}

static void fail(const char *why)
{
    ESP_LOGW(TAG, "turn failed: %s", why);
    end_turn();
    strlcpy(s_turn.error, why, sizeof(s_turn.error));
    emit(MUSE_HATCH_EV_ERROR, why);
}

static void send_fail(const char *context)
{
    char why[EV_TEXT];
    snprintf(why, sizeof(why), "%s: %s", context, muse_link_req_error());
    fail(why);
}

static void response_fail(int status)
{
    if (status == -3) fail("QUEUE BUSY");
    else if (status == -4) fail("PROTOCOL ERROR");
    else if (status <= 0 || status == 200) fail("LOST CONNECTION TO MUSE");
    else if (status == 401) fail("MUSE AUTH REQUIRED (401)");
    else if (status == 403) fail("MUSE ACCESS DENIED (403)");
    else {
        char why[EV_TEXT];
        snprintf(why, sizeof(why), "MUSE %s ERROR (HTTP %d)", status >= 500 ? "SERVER" : "REQUEST", status);
        fail(why);
    }
}

/* ---- The note ---- */

/* Sends the staged PCM as one body chunk; whole chunks are a multiple of 3 bytes, so only the last pads. */
static bool send_stage(bool last)
{
    size_t n = muse_hatch_base64(s_turn.stage, s_turn.stage_len, (char *)s_turn.chunk);
    if (last) {
        memcpy(s_turn.chunk + n, MUSE_HATCH_NOTE_TAIL, sizeof(MUSE_HATCH_NOTE_TAIL) - 1);
        n += sizeof(MUSE_HATCH_NOTE_TAIL) - 1;
    }
    s_turn.stage_len = 0;
    return muse_link_req_send(s_stream[RX_NOTE], s_turn.chunk, n, last, SEND_WAIT_MS);
}

/* ---- The reply ---- */

static void on_ack(void)
{
    rx_t *rx = &s_rx[RX_NOTE];
    if (rx->status != 200 || rx->overflow) {
        ESP_LOGW(TAG, "chat/stream: %d", rx->status);
        if (rx->overflow) fail("INVALID RESPONSE");
        else response_fail(rx->status);
        return;
    }
    cJSON *root = cJSON_Parse(rx->body);
    cJSON *result = cJSON_GetObjectItem(root, "result");
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_IsObject(result) ? result : root, "message_id"));
    strlcpy(s_turn.note_id, id ? id : "", sizeof(s_turn.note_id));
    const char *parent = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_IsObject(result) ? result : root, "reply_to_message_id"));
    strlcpy(s_turn.parent_id, parent ? parent : "", sizeof(s_turn.parent_id));
    cJSON_Delete(root);
    if (!s_turn.note_id[0]) {
        ESP_LOGW(TAG, "chat/stream ack without a message id");
        fail("INVALID RESPONSE");
        return;
    }
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    strlcpy(s_note_id, s_turn.note_id, sizeof(s_note_id));
    strlcpy(s_parent_id, s_turn.parent_id, sizeof(s_parent_id));
    unsigned kept = 0;
    s_chain_count = 0; /* discard provisional parents from unrelated early traffic */
    for (unsigned i = 0; i < s_pending_count; i++) {
        if (related(&s_pending[i])) {
            if (strcmp(s_pending[i].event, "message.user")) remember_parent(s_pending[i].msg);
            s_pending[kept++] = s_pending[i];
        }
    }
    s_pending_count = kept;
    if (!related(&s_delta)) memset(&s_delta, 0, sizeof(s_delta));
    xSemaphoreGive(s_rx_lock);
    s_turn.phase = T_REPLY;
    emit(MUSE_HATCH_EV_SENT, NULL);
}

/* Handles a completed subscription row on the voice task. */
static void on_row(row_t *r)
{
    if (!strcmp(r->event, "message.user")) {
        s_turn.after_note = !strcmp(r->msg, s_turn.note_id) || !strcmp(r->msg, s_turn.parent_id);
    }
    if (s_turn.after_note && !strcmp(r->event, "message.user")) {
        /* The row reads "<transcript>\n[file:audio/wav ...]", or "[Voice note]" before transcription. */
        char *heard = r->text;   /* the voice task owns this dequeued row */
        char *att = strstr(heard, "\n[file:");
        if (att) {
            *att = '\0';
        }
        if (heard[0] && strcmp(heard, "[Voice note]") && !s_turn.heard) {
            char line[EV_TEXT];
            muse_hatch_tail_words(heard, line, sizeof(line));
            emit(MUSE_HATCH_EV_HEARD, line);
            s_turn.heard = true;
        }
        return;
    }
    /* Correlation was checked under the receive lock, including assistant
     * parent chains. Consume the newest completed message, not an ever-growing
     * concatenation that can fill with an interim "still working" response. */
    if (strcmp(r->event, "message.assistant") || !r->ready || !r->text[0]) return;
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)r->text; *p; p++)
        hash = (hash ^ *p) * 16777619u;
    unsigned slot = s_turn.seen_count;
    for (unsigned i = 0; i < s_turn.seen_count; i++) {
        if (!strcmp(r->msg, s_turn.seen[i])) {
            if (hash == s_turn.seen_hash[i]) return;
            slot = i; break; /* a persisted message may revise its completed text */
        }
    }
    if (slot == s_turn.seen_count) {
        if (s_turn.seen_count == 2) {
            memcpy(s_turn.seen[0], s_turn.seen[1], sizeof(s_turn.seen[0]));
            s_turn.seen_hash[0] = s_turn.seen_hash[1];
            s_turn.seen_count = 1;
        }
        slot = s_turn.seen_count++;
    }
    strlcpy(s_turn.seen[slot], r->msg, sizeof(s_turn.seen[slot]));
    s_turn.seen_hash[slot] = hash;
    strlcpy(s_turn.text, r->text, sizeof(s_turn.text));
    s_turn.replied = true;
    s_turn.t_reply = esp_timer_get_time();
    ESP_LOGI(TAG, "reply received after %.2fs bytes=%u", (s_turn.t_reply - s_turn.t_end) / 1e6,
             (unsigned)strlen(s_turn.text)); /* never log the user's message */
    emit(MUSE_HATCH_EV_REPLY, NULL); /* worker reads the complete bounded text */
}

static void subscription_error(int status)
{
    response_fail(status);
}

/* Moves the turn along; runs on the voice task each time it asks for events. */
static void pump(void)
{
    int64_t now = esp_timer_get_time();
    if (s_turn.phase == T_ACK && received(RX_NOTE)) {
        on_ack();
    }
    if (s_turn.phase == T_IDLE) return;
    /* Preserve early events until the POST ACK supplies the note IDs. */
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    int status = s_rx[RX_SUB].status;
    bool closed = s_rx[RX_SUB].done;
    bool early_evicted = s_early_evicted;
    bool skipped_big = s_skipped_big;
    bool receive_oom = s_receive_oom;
    xSemaphoreGive(s_rx_lock);
    if (status >= 400 || (status < 0 && s_turn.phase != T_REPLY)) {
        subscription_error(status);
        return;
    }
    if (s_turn.phase == T_REPLY) {
        for (;;) {
            xSemaphoreTake(s_rx_lock, portMAX_DELAY);
            bool have = s_pending_count > 0;
            if (have) {
                s_pump_pending = s_pending[0];
                if (--s_pending_count) s_pending[0] = s_pending[1];
            }
            xSemaphoreGive(s_rx_lock);
            if (!have) break;
            on_row(&s_pump_pending);
        }
        if (closed || status < 0 || !s_stream[RX_SUB]) {
            if (!s_turn.reconnecting) {
                s_turn.reconnecting = true;
                s_turn.t_disconnected = now;
                s_turn.t_retry = now + SYNC_RETRY_US;
                emit(MUSE_HATCH_EV_SYNCING, NULL);
                ESP_LOGW(TAG, "reply subscription interrupted status=%d; reconnecting", status);
            }
            if (now - s_turn.t_disconnected > SYNC_TIMEOUT_US) {
                fail("MUSE REPLY SYNC INTERRUPTED"); return;
            }
            if (now >= s_turn.t_retry && muse_hatch_ready()) {
                /* Request cancellation must not clear this turn's ACK IDs. */
                if (s_stream[RX_SUB]) muse_link_req_cancel(s_stream[RX_SUB]);
                s_stream[RX_SUB] = 0;
                bool opened = request(RX_SUB, "POST", "/chat/subscribe", true, false);
                if (opened && !muse_link_req_send(s_stream[RX_SUB], "{}", 2, true, SEND_WAIT_MS)) {
                    muse_link_req_cancel(s_stream[RX_SUB]); s_stream[RX_SUB] = 0;
                }
                s_turn.t_retry = now + SYNC_RETRY_US;
            }
            return;
        }
        if (s_turn.reconnecting && status == 0) {
            if (now - s_turn.t_disconnected > SYNC_TIMEOUT_US) fail("MUSE REPLY SYNC INTERRUPTED");
            return;
        }
        if (s_turn.reconnecting && status == 200) {
            s_turn.reconnecting = false;
            ESP_LOGI(TAG, "reply subscription resumed");
            emit(MUSE_HATCH_EV_RESUMED, NULL);
        }
    }
    if (s_turn.phase != T_REPLY) {
        if (s_turn.phase == T_ACK && now - s_turn.t_end > REPLY_TIMEOUT_US) {
            fail("NO REPLY FROM MUSE");
        }
        return;
    }
    /* There is no task-complete signal in a short reply. Keep subscribing until
     * a new voice turn, explicit cancellation, or leaving the application. */
    if (!s_turn.replied && now - s_turn.t_end > REPLY_TIMEOUT_US) {
        fail(receive_oom ? "REPLY OUT OF MEMORY" : skipped_big ? "REPLY EVENT TOO LARGE" : early_evicted ? "REPLY BUFFER LIMIT - TRY AGAIN" : "NO REPLY FROM MUSE");
    }
}

/* ---- Public ---- */

void muse_hatch_start(void)
{
    if (!s_turn_mem) s_turn_mem = calloc(1, sizeof(*s_turn_mem));
    if (!s_turn_mem) return;
    if (!s_mem) s_mem = chat_mem_new();
    if (!s_rx_lock) s_rx_lock = xSemaphoreCreateMutexStatic(&s_rx_lock_storage);
    if (!s_events) s_events = xQueueCreateStatic(8, sizeof(ev_t), s_event_items, &s_event_storage);
    if (!s_mem || !s_rx_lock || !s_events) return;
    s_rx[RX_NOTE].body = s_mem->ack;
    s_rx[RX_NOTE].cap = ACK_MAX;
}

void muse_hatch_stop(void)
{
    if (s_turn_mem && s_rx_lock && s_events) {
        end_turn();
        xQueueReset(s_events);
    }
    // Keep the tiny queue/mutex for reuse. The large parser/turn area below is
    // the material cost and is safe to release only after callbacks are gone.
    memset(s_rx, 0, sizeof(s_rx));
    memset(s_stream, 0, sizeof(s_stream));
    chat_mem_free(s_mem);
    s_mem = NULL;
    free(s_turn_mem);
    s_turn_mem = NULL;
    s_pending_count = 0;
    ESP_LOGI(TAG, "Muse chat scratch released");
}

bool muse_hatch_initialized(void) { return s_mem && s_turn_mem && s_rx_lock && s_events; }

/* Owner task only, after the old session and all its callbacks have stopped.
 * Retain the small turn/correlation state so a delayed answer can still sync. */
void muse_hatch_suspend(void)
{
    if (!muse_hatch_initialized()) return;
    pump();
    if (s_turn.phase == T_TALKING || s_turn.phase == T_ACK)
        fail("LOST CONNECTION TO MUSE"); /* never automatically resend a note */
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    for (int i = 0; i < RX_COUNT; ++i) {
        ++s_rx[i].gen;
        if (i == RX_SUB) row_clear(&s_rx[i]);
        s_rx[i].body = NULL;
        s_rx[i].len = 0;
        s_rx[i].done = true;
        s_stream[i] = 0;
    }
    s_pending_count = 0;
    chat_mem_free(s_mem);
    s_mem = NULL;
    xSemaphoreGive(s_rx_lock);
}

void muse_hatch_status(muse_hatch_status_t *out)
{
    if (!muse_link_hatch_linked()) {
        out->state = MUSE_HATCH_NOT_SET;
        strlcpy(out->detail, "Pair in the Muse app", sizeof(out->detail));
    } else if (!muse_wifi_connected()) {
        out->state = MUSE_HATCH_OFFLINE;
        strlcpy(out->detail, "Waiting for Wi-Fi", sizeof(out->detail));
    } else if (muse_link_req_ready()) {
        out->state = MUSE_HATCH_REACHABLE;
        strlcpy(out->detail, "Through Home Link, text replies", sizeof(out->detail));
    } else {
        out->state = MUSE_HATCH_TESTING;
        strlcpy(out->detail, "Connecting...", sizeof(out->detail));
    }
}

void muse_hatch_test(void)
{
}

void muse_hatch_config_changed(void)
{
}

void muse_hatch_set_resting(bool resting)
{
    (void)resting;   /* Link's session does the polling */
}

const char *muse_hatch_state_name(muse_hatch_state_t state)
{
    switch (state) {
    case MUSE_HATCH_NOT_SET: return "Not set up";
    case MUSE_HATCH_OFFLINE: return "Offline";
    case MUSE_HATCH_UNTESTED: return "Saved";
    case MUSE_HATCH_TESTING: return "Connecting";
    case MUSE_HATCH_REACHABLE: return "Connected";
    case MUSE_HATCH_UNREACHABLE: return "Can't connect";
    }
    return "";
}

bool muse_hatch_ready(void)
{
    return muse_hatch_initialized() && muse_link_hatch_linked() && muse_wifi_connected() && muse_link_req_ready();
}

void muse_hatch_turn_begin(void)
{
    if (!muse_hatch_initialized()) return;
    end_turn();
    xQueueReset(s_events);
    memset(&s_turn, 0, sizeof(s_turn));
    if (!muse_hatch_ready()) { fail("NOT READY"); return; }
    s_turn.stage = malloc(STAGE_BYTES);
    s_turn.chunk = malloc(CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL));
    if (!s_turn.stage || !s_turn.chunk) {
        fail("OUT OF MEMORY");
        return;
    }
    if (!request(RX_NOTE, "POST", "/chat/stream", true, false)
        || !muse_link_req_send(s_stream[RX_NOTE], MUSE_HATCH_NOTE_HEAD, sizeof(MUSE_HATCH_NOTE_HEAD) - 1, false, SEND_WAIT_MS)) {
        send_fail("CAN'T REACH MUSE");
        return;
    }
    muse_hatch_wav_header(s_turn.stage, MIC_RATE);
    s_turn.stage_len = MUSE_HATCH_WAV_HEADER;
    s_turn.phase = T_TALKING;
}

void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    if (!muse_hatch_initialized()) return;
    const uint8_t *p = (const uint8_t *)pcm;
    size_t n = frames * 2;
    while (s_turn.phase == T_TALKING && n) {
        size_t take = STAGE_BYTES - s_turn.stage_len < n ? STAGE_BYTES - s_turn.stage_len : n;
        memcpy(s_turn.stage + s_turn.stage_len, p, take);
        s_turn.stage_len += take;
        p += take;
        n -= take;
        if (s_turn.stage_len == STAGE_BYTES && !send_stage(false)) {
            send_fail("CAN'T KEEP UP");
        }
    }
}

bool muse_hatch_turn_recording(void) { return s_mem && s_turn.phase == T_TALKING; }

void muse_hatch_turn_end(void)
{
    if (!muse_hatch_initialized()) return;
    if (s_turn.phase != T_TALKING) {
        /* Failed while recording: that error went to the recording caption. */
        emit(MUSE_HATCH_EV_ERROR, s_turn.error[0] ? s_turn.error : "CAN'T REACH MUSE");
        return;
    }
    /* Subscribe only after recording: keep inbound reply traffic out of the
     * real-time audio upload. Queue it before the note's final body chunk. */
    if (!request(RX_SUB, "POST", "/chat/subscribe", true, false)
        || !muse_link_req_send(s_stream[RX_SUB], "{}", 2, true, SEND_WAIT_MS)) {
        send_fail("CAN'T SUBSCRIBE TO MUSE");
        return;
    }
    if (!send_stage(true)) {
        send_fail("CAN'T KEEP UP");
        return;
    }
    free(s_turn.stage);
    free(s_turn.chunk);
    s_turn.stage = NULL;
    s_turn.chunk = NULL;
    s_turn.t_end = esp_timer_get_time();
    s_turn.phase = T_ACK;
}

void muse_hatch_turn_cancel(void)
{
    if (!s_turn_mem || !s_rx_lock || !s_events) return;
    end_turn();
    xQueueReset(s_events);
}

muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    if (!s_turn_mem || !s_events) return MUSE_HATCH_EV_NONE;
    if (s_mem) pump();
    else if (s_turn.phase == T_REPLY && s_turn.reconnecting
             && esp_timer_get_time() - s_turn.t_disconnected > SYNC_TIMEOUT_US)
        fail("MUSE REPLY SYNC INTERRUPTED");
    ev_t ev;
    if (xQueueReceive(s_events, &ev, 0) != pdTRUE) {
        return MUSE_HATCH_EV_NONE;
    }
    muse_hatch_tail_words(ev.text, text, cap);
    return ev.type;
}

bool muse_hatch_turn_reply(char *out, size_t cap)
{
    if (!s_turn_mem || !cap || !s_turn.replied) return false;
    strlcpy(out, s_turn.text, cap);
    size_t n = strlen(out);
    if (strlen(s_turn.text) >= cap) {
        while (n && ((unsigned char)s_turn.text[n] & 0xc0) == 0x80) n--;
        out[n] = 0;
    }
    return n > 0;
}

bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    if (!s_mem) return false;
    (void)played;   /* no speech: the reply's page follows the reading pace */
    size_t len = strlen(s_turn.text);
    if (s_turn.phase != T_REPLY || !s_turn.replied || !len) {
        return false;
    }
    return muse_hatch_caption_at(s_turn.text, 0, out, cap);
}

size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    (void)pcm;
    (void)frames;
    if (wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(wait_ms) ? pdMS_TO_TICKS(wait_ms) : 1);
    }
    return 0;
}

size_t muse_hatch_mp3_selftest(int16_t **pcm)
{
    *pcm = NULL;
    return 0;
}
