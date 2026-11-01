"""Apply the small Bunty integration hooks to the checksum-pinned SDK cache."""
from pathlib import Path
import json
import re
ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / '.deps/muse/esp32'

def replace(path, old, new):
    file = SDK / path
    text = file.read_text()
    if new in text:
        return
    if text.count(old) != 1:
        raise RuntimeError(f'Muse integration patch no longer matches: {path}')
    file.write_text(text.replace(old, new))

replace('main/wifi_mgr.c', 'ESP_ERROR_CHECK(esp_event_loop_create_default());',
        'esp_err_t event_err = esp_event_loop_create_default();\n    if (event_err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(event_err);')
replace('main/Kconfig.projbuild', 'depends on (HOMEHUB_LED_BACKEND_VOICE_RING || HOMEHUB_LED_BACKEND_RESPEAKER_LITE) && SPIRAM',
        'depends on (HOMEHUB_LED_BACKEND_VOICE_RING || HOMEHUB_LED_BACKEND_RESPEAKER_LITE || BUNTY_MUSE) && SPIRAM')
replace('main/app.c', '    ESP_LOGI(TAG, "  SDK token:  %.12s", identity_sdk_token() ? identity_sdk_token() : "(none)");',
        '    ESP_LOGI(TAG, "  SDK token configured: %s", identity_sdk_token() ? "yes" : "no");')
replace('main/app.c', '#include "app.h"', '#include "app.h"\n#include "BuntyMuseHooks.h"')
if 'strncmp(command, "bunty.", 6)' not in (SDK / 'main/app.c').read_text():
    replace('main/app.c', '    if (strcmp(command, "device.list_vms") == 0) {',
            '    if (strncmp(command, "bunty.", 6) == 0) return bunty_muse_command(command, params);\n    if (strcmp(command, "device.list_vms") == 0) {')
# Bunty has a framebuffer and audio DMA like the SDK's display boards. Keep
# TLS out of BLE provisioning, then reboot once with BLE absent from startup.
replace('main/app.c',
        '    if (strncmp(command, "bunty.", 6) == 0) return bunty_muse_command(command, params);',
        '    if (strncmp(command, "bunty.", 6) == 0 || strncmp(command, "ui.", 3) == 0) return bunty_muse_command(command, params);')
replace('main/app.c', '#define PAIR_THEN_RESTART CONFIG_MUSE_ENABLED',
        '#define PAIR_THEN_RESTART (CONFIG_MUSE_ENABLED || CONFIG_BUNTY_MUSE)')
file = SDK / 'main/noise_control.cpp'
text = file.read_text()
if 'BuntyMuseHooks.h' not in text:
    text = text.replace('#include "noise_control.h"', '#include "noise_control.h"\n#include "BuntyMuseHooks.h"')
    # Enable HTTP-over-Noise streams while retaining our own UI instead of LVGL.
    text = text.replace('#if CONFIG_MUSE_ENABLED', '#if (CONFIG_MUSE_ENABLED || CONFIG_BUNTY_MUSE)')
    text = text.replace('    cJSON_AddItemToObject(params, "commands_v2", commands);',
                        '    bunty_muse_register(commands);\n    cJSON_AddItemToObject(params, "commands_v2", commands);')
    file.write_text(text)
file = SDK / 'main/noise_tunnel.cpp'
text = file.read_text()
text = text.replace('#if CONFIG_MUSE_ENABLED', '#if (CONFIG_MUSE_ENABLED || CONFIG_BUNTY_MUSE)')
if file.read_text() != text:
    file.write_text(text)
replace('components/muse/muse_chat_link.c', '#include "muse_chat.h"',
        '#include "muse_chat.h"\n#include "BuntyMuseHooks.h"')
# Expose checked initialization without changing the upstream void API. A
# failed queue/mutex allocation must not become an assertion on the first reply.
replace('components/muse/muse_chat_link.c',
        'void muse_hatch_status(muse_hatch_status_t *out)',
        '\n'.join([
            'bool bunty_muse_chat_init(void)', '{',
            '    if (s_rx_lock && s_events) return true;',
            '    muse_hatch_start();',
            '    if (s_rx_lock && s_events) return true;',
            '    if (s_rx_lock) vSemaphoreDelete(s_rx_lock);',
            '    if (s_events) vQueueDelete(s_events);',
            '    s_rx_lock = NULL; s_events = NULL;',
            '    return false;', '}', '',
            'void muse_hatch_status(muse_hatch_status_t *out)']))
replace('components/muse/muse_chat_link.c', '        text_append(s_turn.text, sizeof(s_turn.text), r->text);',
        '        text_append(s_turn.text, sizeof(s_turn.text), r->text);\n        bunty_muse_reply(r->text);')

# Bunty renders its own captions and blocks on drained TTS playback. Do not
# hold the next listening turn for the SDK's separate text-reading timer.
replace('components/muse/muse_chat_link.c',
        'if (scroll(now) && now - s_turn.t_reply > SETTLE_US)',
        'if ((scroll(now) || CONFIG_BUNTY_MUSE) && now - s_turn.t_reply > SETTLE_US)')

replace('main/Kconfig.projbuild', 'config HOMEHUB_DISPLAY_COMMANDS\n        bool\n        default y if HOMEHUB_DISPLAY\n',
        'config HOMEHUB_DISPLAY_COMMANDS\n        bool\n        default y if HOMEHUB_DISPLAY || BUNTY_MUSE\n')
replace('main/image_fetch.c', '#include "sdkconfig.h"',
        '#include "sdkconfig.h"\n#if CONFIG_BUNTY_MUSE\n#include "BuntyMuseHooks.h"\n#endif')
replace('main/image_fetch.c', '#if CONFIG_HOMEHUB_LED_BACKEND_MUSE\n#define CENTRED',
        '#if CONFIG_HOMEHUB_LED_BACKEND_MUSE || CONFIG_BUNTY_MUSE\n#define CENTRED')
replace('main/image_fetch.h', '#if CONFIG_HOMEHUB_LED_BACKEND_MUSE\n#define IMAGE_FETCH_DEFAULT_ROW',
        '#if CONFIG_HOMEHUB_LED_BACKEND_MUSE || CONFIG_BUNTY_MUSE\n#define IMAGE_FETCH_DEFAULT_ROW')
replace('main/image_fetch.c', '    while (have < len && !f->eof) {',
        '    while (have < len && !f->eof) {\n#if CONFIG_BUNTY_MUSE\n        if (!bunty_muse_image_current()) return -1;\n#endif')
replace('main/image_fetch.c', '        // Show what arrived, even if the download broke off.',
        '        if (!fail && result.height <= 0) fail = "empty image";\n        // Show what arrived, even if the download broke off.')
replace('main/image_fetch.c', '    result.bytes = f->bytes;',
        '#if CONFIG_BUNTY_MUSE\n    if (!bunty_muse_image_current()) {\n        result.ok = false; result.code = "cancelled"; result.message = "image dismissed";\n    }\n    if (!bunty_muse_image_end(result.ok) && result.ok) {\n        result.ok = false; result.code = "cancelled"; result.message = "image dismissed";\n    }\n#endif\n    result.bytes = f->bytes;')
replace('main/image_fetch.c', '    if (xTaskCreate(fetch_task, "image", stack, f, 4, NULL) != pdPASS) {',
        '#if CONFIG_BUNTY_MUSE\n    if (!bunty_muse_image_begin()) {\n        free(f->url); free(f); atomic_store(&s_busy, false);\n        *code = "busy"; *message = "display or speech download is busy"; return false;\n    }\n#endif\n    if (xTaskCreate(fetch_task, "image", stack, f, 4, NULL) != pdPASS) {\n#if CONFIG_BUNTY_MUSE\n        bunty_muse_image_end(false);\n#endif')
replace('main/app.c', '#include "BuntyMuseHooks.h"', '#include "BuntyMuseHooks.h"\n#include <math.h>')
replace('main/app.c', '    if (strcmp(command, "display.show_animation") == 0) {',
        '    if (strcmp(command, "display.show_text") == 0 || strcmp(command, "display.info") == 0) return bunty_muse_command(command, params);\n    if (strcmp(command, "display.show_animation") == 0) {')
# Correct remote parameter validation before integer conversion, including NaN,
# fractions, oversized URLs and rows outside the panel. The downloader checks bounds.
replace('main/app.c', '        draw_url_ctx_t *ctx = calloc(1, sizeof(*ctx));',
        '        if (strlen(url->valuestring) > 2048) return command_error("invalid_params", "url exceeds 2048 bytes");\n        if (row && (!cJSON_IsNumber(row) || !isfinite(row->valuedouble) || row->valuedouble != row->valueint)) return command_error("invalid_params", "row must be an integer");\n        draw_url_ctx_t *ctx = calloc(1, sizeof(*ctx));')

token_file = ROOT / '.muse-token'
token = token_file.read_text().strip() if token_file.exists() else ''
if token and not re.fullmatch(r'mgst_[A-Za-z0-9_-]+', token):
    raise RuntimeError('Invalid Muse SDK token format in firmware/.muse-token')
header = ROOT / '.deps/muse-token.h'
content = '#include "sdkconfig.h"\n#undef CONFIG_GADGET_SDK_TOKEN\n#define CONFIG_GADGET_SDK_TOKEN ' + json.dumps(token) + '\n'
if not header.exists() or header.read_text() != content:
    header.write_text(content)
header.chmod(0o600)

# Keep voice audio and gadget instructions in the same request. Only Bunty
# changes the message; generic SDK boards retain their original empty message.
if '#if CONFIG_BUNTY_MUSE\n#define MUSE_HATCH_NOTE_HEAD' not in (SDK / 'components/muse/muse_chat_priv.h').read_text():
    replace('components/muse/muse_chat_priv.h', '#define MUSE_HATCH_NOTE_HEAD \\\n    "{\\"message\\":\\"\\",\\"output_modality\\":\\"text\\",\\"items\\":[{\\"type\\":\\"file\\"," \\\n    "\\"mime_type\\":\\"audio/wav\\",\\"filename\\":\\"voice_note.wav\\",\\"data_base64\\":\\""',
            '#if CONFIG_BUNTY_MUSE\n#define MUSE_HATCH_NOTE_HEAD \\\n    "{\\"message\\":\\"This voice note is from Bunty, a small speaking gadget. Reply naturally and concisely. Along with the spoken reply, choose a suitable servo movement by calling bunty.gesture before replying: neutral, nod, shake, curious or happy. Use neutral if no movement suits the reply. The device controls safe angles, speed and power; do not use bunty.look during conversation. Keep movement instructions and tool markup out of the spoken text. When asked to show calendar events, tasks, notes or information, use the registered ui tools with real available data. Do not invent events or claim a provider update succeeded without actually updating it.\\",\\"output_modality\\":\\"text\\",\\"items\\":[{\\"type\\":\\"file\\",\\"mime_type\\":\\"audio/wav\\",\\"filename\\":\\"voice_note.wav\\",\\"data_base64\\":\\""\n#else\n#define MUSE_HATCH_NOTE_HEAD \\\n    "{\\"message\\":\\"\\",\\"output_modality\\":\\"text\\",\\"items\\":[{\\"type\\":\\"file\\"," \\\n    "\\"mime_type\\":\\"audio/wav\\",\\"filename\\":\\"voice_note.wav\\",\\"data_base64\\":\\""\n#endif')

replace('components/muse/muse_chat_priv.h', '#include "muse_chat.h"',
        '#ifdef ESP_PLATFORM\n#include "sdkconfig.h"\n#endif\n#include "muse_chat.h"')

# Serialize the expanded Bunty tool registry without consuming internal RAM.
replace('main/noise_control.cpp', '    char *json = nullptr;\n    for (int size = 2048; size <= 8192 && !json; size += 512) {\n        json = (char *)malloc(size);\n        if (!json) break;\n        if (!cJSON_PrintPreallocated(root, json, size, false)) {\n            free(json);\n            json = nullptr;\n        }\n    }',
        '    char *json = nullptr;\n#if CONFIG_BUNTY_MUSE\n    // Structured screens expand commands_v2 past the upstream 8 KB cap.\n    // One bounded PSRAM buffer preserves internal heap for TLS and DMA.\n    constexpr int registration_capacity = 32768;\n    json = (char *)heap_caps_malloc(registration_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);\n    if (json && !cJSON_PrintPreallocated(root, json, registration_capacity, false)) {\n        free(json);\n        json = nullptr;\n    }\n#else\n    for (int size = 2048; size <= 8192 && !json; size += 512) {\n        json = (char *)malloc(size);\n        if (!json) break;\n        if (!cJSON_PrintPreallocated(root, json, size, false)) {\n            free(json);\n            json = nullptr;\n        }\n    }\n#endif')

replace('components/muse/muse_chat_link.c', 'static uint64_t s_last_seq;',
        'static uint64_t s_last_seq;\nstatic int64_t s_reply_progress_us;')

# A transport end can race the final row or arrive before the upload ACK.
# Drain correlated finals first. Once a complete reply was delivered, a closed
# subscription is completion; authorization/server errors remain failures.
replace('components/muse/muse_chat_link.c',
        '    if (status < 0 || status >= 400) {\n        subscription_error(status);\n        return;\n    }\n    if (s_turn.phase == T_REPLY) {',
        '    if ((status < 0 || status >= 400) && s_turn.phase != T_REPLY && !have_pending) {\n        subscription_error(status);\n        return;\n    }\n    if (s_turn.phase == T_REPLY) {')
replace('components/muse/muse_chat_link.c',
        '        if (closed && !s_turn.replied) {\n            subscription_error(status);\n            return;\n        }',
        '        if (status >= 400 || ((closed || status < 0) && !s_turn.replied)) {\n            subscription_error(status);\n            return;\n        }\n        if (s_turn.replied && (closed || status < 0)) {\n            end_turn();\n            emit(MUSE_HATCH_EV_DONE, NULL);\n            return;\n        }')
# Keep production logs free of spoken content.
replace('components/muse/muse_chat_link.c',
        '        ESP_LOGI(TAG, "reply after %.2fs: %.80s", (esp_timer_get_time() - s_turn.t_end) / 1e6, r->text);',
        '        ESP_LOGI(TAG, "reply after %.2fs: %u bytes", (esp_timer_get_time() - s_turn.t_end) / 1e6, (unsigned)strlen(r->text));')

replace('components/muse/muse_chat_link.c',
        '    bool early_evicted = s_early_evicted;',
        '    bool have_pending = s_pending_count > 0;\n    int64_t progress = s_reply_progress_us;\n    bool early_evicted = s_early_evicted;')

# Sniff GIF signatures before falling back to raw RGB565. Reuse the bounded
# image downloader, its redirects/deadline, network lease and cancellation.
replace('main/image_fetch.c', 'static void fetch_task(void *arg) {',
        '#if CONFIG_BUNTY_MUSE\nstatic int gif_read(void *context, uint8_t *data, size_t bytes) {\n    return fetch_read((fetch_t *)context, data, bytes);\n}\n#endif\n\nstatic void fetch_task(void *arg) {')
replace('main/image_fetch.c',
        '            fail = draw_jpeg(f, &result.width, &result.height, &result.scale);\n        } else {',
        '            fail = draw_jpeg(f, &result.width, &result.height, &result.scale);\n#if CONFIG_BUNTY_MUSE\n        } else if (f->peek_len == 6 && (!memcmp(f->peek, "GIF87a", 6) || !memcmp(f->peek, "GIF89a", 6))) {\n            result.format = "gif"; result.scale = 1;\n            fail = bunty_muse_gif_load(gif_read, f, f->row, &result.width, &result.height);\n#endif\n        } else {')

# A long streamed answer is not a silent timeout. Extend only on explicitly
# correlated assistant progress, with a separate three-minute total ceiling.
replace('components/muse/muse_chat_link.c', '    if (start || append) {',
        '    if ((start || append) && s_note_id[0]) {\n        const char *parent = s_row.reply_to[0] ? s_row.reply_to\n            : !strcmp(s_row.msg, s_delta.msg) ? s_delta.reply_to : "";\n        if (parent[0] && (!strcmp(parent, s_note_id) || !strcmp(parent, s_parent_id)))\n            s_reply_progress_us = esp_timer_get_time();\n    }\n    if (start || append) {')
replace('components/muse/muse_chat_link.c', '        s_last_seq = 0;',
        '        s_last_seq = 0; s_reply_progress_us = 0;')
replace('components/muse/muse_chat_link.c',
        '    } else if (now - s_turn.t_end > REPLY_TIMEOUT_US) {',
        '    } else if (now - s_turn.t_end > 180000000LL ||\n               now - (progress > s_turn.t_end ? progress : s_turn.t_end) > REPLY_TIMEOUT_US) {')

replace('main/image_fetch.c', 'uint8_t peek[2];', 'uint8_t peek[6];')
replace('main/image_fetch.c', '        f->peek[0] = f->peek[1];\n        f->peek_len--;',
        '        f->peek_len--;\n        memmove(f->peek, f->peek + 1, f->peek_len);')
replace('main/image_fetch.c', 'f->peek_len == 2 && f->peek[0] == 0xFF',
        'f->peek_len >= 2 && f->peek[0] == 0xFF')
# A deadline reached inside the signature read is still a deadline failure.
replace('main/image_fetch.c', '        if (n < 0) return -1;',
        '        if (n < 0) {\n            if (esp_timer_get_time() >= f->deadline_us) f->timed_out = true;\n            return -1;\n        }')

# The end-of-utterance subscription is queued behind the last audio chunks.
# A zero-wait Open rejects a healthy but temporarily full queue. Admission is
# bounded and does not resend a request or grow the queue.
replace('main/noise_control.cpp', '    if (!req_put(op, 0)) {',
        '    if (!req_put(op, pdMS_TO_TICKS(CONFIG_BUNTY_MUSE ? 1000 : 0))) {')
replace('components/muse/muse_chat_link.c', '#define SEND_WAIT_MS 200',
        '#define SEND_WAIT_MS (CONFIG_BUNTY_MUSE ? 5000 : 200)')
# Safe metadata lets us distinguish transport failures from rejected ACK shape.
replace('components/muse/muse_chat_link.c', '    rx_t *rx = &s_rx[RX_NOTE];\n    if (rx->status != 200',
        '    rx_t *rx = &s_rx[RX_NOTE];\n    ESP_LOGI(TAG, "note ACK: status=%d bytes=%u overflow=%d", rx->status, (unsigned)rx->len, rx->overflow);\n    if (rx->status != 200')

# Bunty uploads a completed bounded recording, so use 8 KiB base64 chunks
# instead of 2 KiB. They fit the existing 12 KiB Noise envelope/16 KiB WS buffers.
# Keep payloads in PSRAM and retain the existing queue count/byte bounds.
replace('components/muse/muse_chat_link.c', '#define STAGE_BYTES 1536',
        '#define STAGE_BYTES (CONFIG_BUNTY_MUSE ? 6144 : 1536)')
replace('components/muse/muse_chat_link.c', '#include "esp_random.h"',
        '#include "esp_random.h"\n#include "esp_heap_caps.h"')
replace('components/muse/muse_chat_link.c', '    s_turn.stage = malloc(STAGE_BYTES);',
        '    s_turn.stage = CONFIG_BUNTY_MUSE ? heap_caps_malloc(STAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : malloc(STAGE_BYTES);')
replace('components/muse/muse_chat_link.c', '    s_turn.chunk = malloc(CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL));',
        '    s_turn.chunk = CONFIG_BUNTY_MUSE ? heap_caps_malloc(CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : malloc(CHUNK_BYTES + sizeof(MUSE_HATCH_NOTE_TAIL));')
replace('main/noise_control.cpp', '    uint8_t *copy = static_cast<uint8_t *>(malloc(len ? len : 1));',
        '    uint8_t *copy = static_cast<uint8_t *>(CONFIG_BUNTY_MUSE\n        ? heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)\n        : malloc(len ? len : 1));')
replace('main/noise_control.cpp',
        '    if (!noise_tx_has_dma_headroom_reclaiming(queued > 0 ? (size_t)queued : 0)\n        || !noise_tx_has_contiguous_dma_headroom()) return true;',
        '    bool margin = CONFIG_BUNTY_MUSE ? noise_tx_has_dma_headroom(nullptr)\n        : noise_tx_has_dma_headroom_reclaiming(queued > 0 ? (size_t)queued : 0);\n    if (!margin || !noise_tx_has_contiguous_dma_headroom()) return true;')

# Queue diagnostics contain counts/heap levels only, never request data.
replace('main/noise_control.cpp',
        '        if (left <= 0) {\n            free(copy);',
        '        if (left <= 0) {\n            ESP_LOGW(TAG, "request TX stalled: byte budget, queued=%ld dma=%u largest=%u", (long)s_req_queued_bytes.load(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));\n            free(copy);')
replace('main/noise_control.cpp',
        '    if (!req_put(op, left > 0 ? pdMS_TO_TICKS(left / 1000) : 0)) {\n        free(copy);',
        '    if (!req_put(op, left > 0 ? pdMS_TO_TICKS(left / 1000) : 0)) {\n        ESP_LOGW(TAG, "request TX stalled: slots, queued=%ld dma=%u largest=%u", (long)s_req_queued_bytes.load(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));\n        free(copy);')

# Keep an accepted voice note while recovering a transient reply stream. Never
# replay /chat/stream (which could execute a user's request twice). Authorization
# and non-transient 4xx responses remain terminal. Recovery is bounded by
# the turn deadline; callbacks from replaced streams are generation-gated.
replace('components/muse/muse_chat_link.c',
        '    int64_t t_end, t_reply, t_show;',
        '    int64_t t_end, t_reply, t_show;\n    int64_t sub_retry_at;\n    unsigned sub_retries;')
if 'static bool recover_subscription(' not in (SDK / 'components/muse/muse_chat_link.c').read_text():
    replace('components/muse/muse_chat_link.c', '/* How far through the reply reading has got. */', r'''static bool recover_subscription(int64_t now, int status, bool closed)
{
    if (!CONFIG_BUNTY_MUSE || s_turn.replied || status == 401 || status == 403
        || (status >= 400 && status < 500 && status != 429 && status != 409)) return false;
    bool transient = status < 0 || status >= 500 || status == 429 || status == 409
        || (closed && status == 200);
    if (!transient && !s_turn.sub_retry_at) return false;
    if (s_turn.sub_retries >= 3 || now - s_turn.t_end >= 180000000LL) return false;
    if (!s_turn.sub_retry_at) {
        s_turn.sub_retry_at = now + (500000LL << s_turn.sub_retries);
        ESP_LOGW(TAG, "reply stream recovery: status=%d attempt=%u", status, s_turn.sub_retries + 1);
        return true;
    }
    if (now < s_turn.sub_retry_at) return true;
    ++s_turn.sub_retries;
    drop(RX_SUB);
    bool opened = request(RX_SUB, "POST", "/chat/subscribe", true, false);
    bool sent = opened && muse_link_req_send(s_stream[RX_SUB], "{}", 2, true, SEND_WAIT_MS);
    s_turn.sub_retry_at = sent ? 0 : now + (500000LL << s_turn.sub_retries);
    if (!sent && s_turn.sub_retries >= 3) {
        fail("CAN'T SUBSCRIBE TO MUSE");
    }
    return true;
}

/* How far through the reply reading has got. */''')
replace('components/muse/muse_chat_link.c',
        '    if ((status < 0 || status >= 400) && s_turn.phase != T_REPLY && !have_pending) {',
        '    if (s_turn.phase == T_ACK && !have_pending && recover_subscription(now, status, closed)) return;\n    if ((status < 0 || status >= 400) && s_turn.phase != T_REPLY && !have_pending) {')
replace('components/muse/muse_chat_link.c',
        '        if (status >= 400 || ((closed || status < 0) && !s_turn.replied)) {',
        '        if (recover_subscription(now, status, closed)) return;\n        if (status >= 400 || ((closed || status < 0) && !s_turn.replied)) {')
# A permission/tool operation may take longer than a silent reply, but retain a
# finite deadline. Do not treat global agent traffic as correlated progress.
replace('components/muse/muse_chat_link.c', '#define REPLY_TIMEOUT_US 60000000',
        '#define REPLY_TIMEOUT_US (CONFIG_BUNTY_MUSE ? 180000000LL : 60000000LL)')
# A complete correlated answer takes precedence over a later transient stream
# failure; retain explicit authorization/protocol failures as failures.
replace('components/muse/muse_chat_link.c',
        '        if (recover_subscription(now, status, closed)) return;',
        '        if (s_turn.replied && (status >= 500 || status == 429 || status == 409)) {\n            end_turn(); emit(MUSE_HATCH_EV_DONE, NULL); return;\n        }\n        if (recover_subscription(now, status, closed)) return;')

# The companion stays quiet unless the user asks for a content view.
if 'Only fetch or show screen content when the user asks' not in (SDK / 'components/muse/muse_chat_priv.h').read_text():
    replace('components/muse/muse_chat_priv.h',
            'Reply naturally and concisely. Along with the spoken reply,',
            'Reply naturally and concisely. Only fetch or show screen content when the user asks for it; show the requested view only. Do not automatically load calendars, tasks, notes, dashboards or unrelated summaries. Keep the companion face for ordinary conversation. Along with the spoken reply,')

# Existing fitting URLs do not need an artifact upload/hosting step. Any actual
# Muse-side hosting approval remains controlled by the app, never by firmware.
replace('components/muse/muse_chat_priv.h',
        'Keep the companion face for ordinary conversation. Along with the spoken reply,',
        'Keep the companion face for ordinary conversation. For requested GIFs or images, use an existing direct URL if it meets the device limits; do not upload or recreate a usable image. Use structured ui tools or display.show_text for information instead of making an image. Any necessary image-hosting approval is handled in the Muse app. Along with the spoken reply,')

# Recover until the bounded turn deadline, without repeating the recording.
replace('components/muse/muse_chat_link.c',
        '    if (s_turn.sub_retries >= 3 || now - s_turn.t_end >= 180000000LL) return false;',
        '    if (now - s_turn.t_end >= 180000000LL) {\n        fail("NO REPLY FROM MUSE"); return true;\n    }')
retry_file = SDK / 'components/muse/muse_chat_link.c'
retry_source = retry_file.read_text()
retry_old = 'now + (500000LL << s_turn.sub_retries)'
retry_new = 'now + (s_turn.sub_retries < 6 ? (500000LL << s_turn.sub_retries) : 30000000LL)'
if retry_old in retry_source:
    if retry_source.count(retry_old) != 2: raise RuntimeError('reply backoff migration mismatch')
    retry_file.write_text(retry_source.replace(retry_old,retry_new))
replace('components/muse/muse_chat_link.c',
        '    if (!sent && s_turn.sub_retries >= 3) {\n        fail("CAN\'T SUBSCRIBE TO MUSE");\n    }',
        '    /* Failed opens retain the retry time; never re-upload the note. */')

# One-shot GIF display goes straight to Bunty, without the legacy transfer flow.
replace('main/app.c',
        '    if (strncmp(command, "bunty.", 6) == 0 || strncmp(command, "ui.", 3) == 0) return bunty_muse_command(command, params);',
        '    if (strcmp(command, "display.show_gif") == 0) return bunty_muse_command(command, params);\n    if (strncmp(command, "bunty.", 6) == 0 || strncmp(command, "ui.", 3) == 0) return bunty_muse_command(command, params);')

# Associate an optional reaction with the URL request itself. Failed/cancelled
# downloads never trigger movement; successful callbacks publish to main loop.
replace('main/app.c', '    char request_id[64];\n} draw_url_ctx_t;',
        '    char request_id[64];\n    int gesture, speed;\n} draw_url_ctx_t;')
replace('main/app.c',
        '        ctx->session_generation = session_generation;\n        strncpy(ctx->request_id, request_id, sizeof(ctx->request_id) - 1);',
        '        if (!bunty_muse_parse_image_gesture(params, &ctx->gesture, &ctx->speed)) { free(ctx); return command_error("invalid_params", "invalid gesture or speed"); }\n        ctx->session_generation = session_generation;\n        strncpy(ctx->request_id, request_id, sizeof(ctx->request_id) - 1);')
replace('main/app.c',
        '        cJSON_AddStringToObject(payload, "format", r->format);',
        '        bunty_muse_image_gesture(ctx->gesture, ctx->speed);\n        cJSON_AddStringToObject(payload, "format", r->format);')

replace('components/muse/muse_chat_priv.h',
        'Keep movement instructions and tool markup out of the spoken text.',
        'Keep movement instructions and tool markup out of the spoken text. For spoken-only replies use bunty.gesture with when speech and speed fast. For meme or GIF requests, instead include gesture and speed in the display.show_gif or display.draw_url call so movement starts with the picture even without speech. Choose movement and tempo to match the meme: happy for funny or excited, curious for confused or surprised, nod for agreement, shake for disapproval, neutral for stillness. Prefer fast for energetic memes and normal for calmer ones; use slow only when deliberate slow motion suits the request.')
