#include "fb_net.h"

#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "fb_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "fb_net";

#define FB_CONNECTED_BIT BIT0
#define JSON_BUF_SIZE 4096

static EventGroupHandle_t s_events;
static volatile bool s_online;
static int s_retries;

/* ------------------------------------------------------------------ wifi */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_online = false;
        xEventGroupClearBits(s_events, FB_CONNECTED_BIT);
        /* Back off gently, but never give up: this device sits in a child's
         * bedroom and has to heal itself after a router reboot with nobody
         * around to press anything. */
        int delay_ms = (s_retries < 6) ? (1000 << s_retries) : 60000;
        s_retries++;
        ESP_LOGW(TAG, "wifi disconnected, retrying in %d ms", delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "online, ip=" IPSTR, IP2STR(&event->ip_info.ip));
        s_retries = 0;
        s_online = true;
        xEventGroupSetBits(s_events, FB_CONNECTED_BIT);
    }
}

esp_err_t fb_net_start(void)
{
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "event group alloc failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL), TAG, "wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL), TAG, "ip handler failed");

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = CONFIG_FB_WIFI_SSID,
            .password = CONFIG_FB_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    ESP_LOGI(TAG, "connecting to \"%s\" (2.4 GHz only)", CONFIG_FB_WIFI_SSID);
    return ESP_OK;
}

bool fb_net_online(void)
{
    return s_online;
}

/* ------------------------------------------------------------------ http */

static esp_http_client_handle_t open_get(const char *path, int *status, int64_t *len)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", FB_RELAY_URL, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return NULL;
    }

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", FB_RELAY_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth);

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    *len = esp_http_client_fetch_headers(client);
    *status = esp_http_client_get_status_code(client);
    return client;
}

static esp_err_t read_all(esp_http_client_handle_t client, uint8_t *buf, size_t cap,
                          size_t *out_len)
{
    size_t total = 0;
    while (total < cap) {
        int n = esp_http_client_read(client, (char *)buf + total, cap - total);
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    *out_len = total;
    return ESP_OK;
}

esp_err_t fb_net_fetch(const char *path, uint8_t *buf, size_t cap, size_t *out_len)
{
    int status = 0;
    int64_t len = 0;
    esp_http_client_handle_t client = open_get(path, &status, &len);
    ESP_RETURN_ON_FALSE(client, ESP_FAIL, TAG, "open %s failed", path);

    esp_err_t err = ESP_OK;
    if (status != 200) {
        ESP_LOGE(TAG, "GET %s -> HTTP %d", path, status);
        err = ESP_FAIL;
    } else if (len > 0 && (size_t)len > cap) {
        ESP_LOGE(TAG, "GET %s is %lld bytes, buffer holds %u", path, (long long)len,
                 (unsigned)cap);
        err = ESP_ERR_NO_MEM;
    } else {
        err = read_all(client, buf, cap, out_len);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

/* ------------------------------------------------------------------ inbox */

esp_err_t fb_net_poll_next(int32_t since, fb_inbox_entry_t *out)
{
    char path[96];
    snprintf(path, sizeof(path), "/api/v1/inbox?since=%ld&limit=1", (long)since);

    char *json = malloc(JSON_BUF_SIZE);
    ESP_RETURN_ON_FALSE(json, ESP_ERR_NO_MEM, TAG, "json buffer alloc failed");

    size_t len = 0;
    esp_err_t err = fb_net_fetch(path, (uint8_t *)json, JSON_BUF_SIZE - 1, &len);
    if (err != ESP_OK) {
        free(json);
        return err;
    }
    json[len] = '\0';

    cJSON *root = cJSON_Parse(json);
    free(json);
    ESP_RETURN_ON_FALSE(root, ESP_FAIL, TAG, "inbox json parse failed");

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    cJSON *msg = (messages && cJSON_IsArray(messages)) ? cJSON_GetArrayItem(messages, 0) : NULL;
    if (!msg) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    memset(out, 0, sizeof(*out));
    cJSON *id = cJSON_GetObjectItem(msg, "id");
    out->id = id ? (int32_t)cJSON_GetNumberValue(id) : 0;

    cJSON *photo = cJSON_GetObjectItem(msg, "photo");
    if (photo && cJSON_IsObject(photo)) {
        cJSON *p = cJSON_GetObjectItem(photo, "path");
        cJSON *b = cJSON_GetObjectItem(photo, "bytes");
        if (p && cJSON_IsString(p)) {
            strlcpy(out->photo_path, p->valuestring, sizeof(out->photo_path));
            out->photo_bytes = b ? (size_t)cJSON_GetNumberValue(b) : 0;
            out->has_photo = true;
        }
    }

    cJSON *audio = cJSON_GetObjectItem(msg, "audio");
    if (audio && cJSON_IsObject(audio)) {
        cJSON *p = cJSON_GetObjectItem(audio, "path");
        cJSON *b = cJSON_GetObjectItem(audio, "bytes");
        cJSON *ms = cJSON_GetObjectItem(audio, "ms");
        if (p && cJSON_IsString(p)) {
            strlcpy(out->audio_path, p->valuestring, sizeof(out->audio_path));
            out->audio_bytes = b ? (size_t)cJSON_GetNumberValue(b) : 0;
            out->audio_ms = ms ? (int32_t)cJSON_GetNumberValue(ms) : 0;
            out->has_audio = true;
        }
    }

    cJSON_Delete(root);
    return (out->id > since) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------- wav */

#define WAV_HEADER_BYTES 44

typedef struct __attribute__((packed)) {
    char riff[4];
    uint32_t riff_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
    char data[4];
    uint32_t data_size;
} wav_header_t;

esp_err_t fb_net_fetch_wav(const char *path, int16_t *buf, size_t max_samples,
                           size_t *out_samples)
{
    int status = 0;
    int64_t len = 0;
    esp_http_client_handle_t client = open_get(path, &status, &len);
    ESP_RETURN_ON_FALSE(client, ESP_FAIL, TAG, "open %s failed", path);

    esp_err_t err = ESP_OK;
    if (status != 200) {
        ESP_LOGE(TAG, "GET %s -> HTTP %d", path, status);
        err = ESP_FAIL;
        goto done;
    }

    /* Walk the RIFF chunks to find `data`. ffmpeg sometimes emits a LIST
     * chunk before it, so a blind 44-byte skip would pull in metadata as
     * audio and produce a burst of noise. */
    char hdr[12];
    if (esp_http_client_read(client, hdr, 12) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "%s is not a RIFF/WAVE file", path);
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    uint32_t data_bytes = 0;
    for (int guard = 0; guard < 16; guard++) {
        char chunk[8];
        if (esp_http_client_read(client, chunk, 8) != 8) {
            err = ESP_ERR_INVALID_RESPONSE;
            goto done;
        }
        uint32_t size;
        memcpy(&size, chunk + 4, 4);
        if (memcmp(chunk, "data", 4) == 0) {
            data_bytes = size;
            break;
        }
        /* Skip this chunk's body (chunks are word-aligned). */
        uint32_t skip = size + (size & 1u);
        char sink[64];
        while (skip > 0) {
            int want = (skip > sizeof(sink)) ? (int)sizeof(sink) : (int)skip;
            int n = esp_http_client_read(client, sink, want);
            if (n <= 0) {
                err = ESP_ERR_INVALID_RESPONSE;
                goto done;
            }
            skip -= (uint32_t)n;
        }
    }

    if (data_bytes == 0) {
        ESP_LOGE(TAG, "%s has no data chunk", path);
        err = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    size_t samples = data_bytes / sizeof(int16_t);
    if (samples > max_samples) {
        ESP_LOGW(TAG, "%s holds %u samples, truncating to %u", path,
                 (unsigned)samples, (unsigned)max_samples);
        samples = max_samples;
    }

    size_t got = 0;
    err = read_all(client, (uint8_t *)buf, samples * sizeof(int16_t), &got);
    *out_samples = got / sizeof(int16_t);

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t fb_net_send_reply(const int16_t *pcm, size_t samples)
{
    const uint32_t data_size = (uint32_t)(samples * sizeof(int16_t));
    wav_header_t hdr = {
        .riff = {'R', 'I', 'F', 'F'},
        .riff_size = 36 + data_size,
        .wave = {'W', 'A', 'V', 'E'},
        .fmt = {'f', 'm', 't', ' '},
        .fmt_size = 16,
        .format = 1,
        .channels = FB_CHANNELS,
        .sample_rate = FB_SAMPLE_RATE,
        .byte_rate = FB_SAMPLE_RATE * FB_CHANNELS * sizeof(int16_t),
        .block_align = FB_CHANNELS * sizeof(int16_t),
        .bits = 16,
        .data = {'d', 'a', 't', 'a'},
        .data_size = data_size,
    };

    char url[256];
    snprintf(url, sizeof(url), "%s/api/v1/reply", FB_RELAY_URL);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    ESP_RETURN_ON_FALSE(client, ESP_FAIL, TAG, "reply client init failed");

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", FB_RELAY_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", "audio/wav");

    char duration[24];
    snprintf(duration, sizeof(duration), "%u", (unsigned)(samples * 1000 / FB_SAMPLE_RATE));
    esp_http_client_set_header(client, "X-Duration-Ms", duration);

    esp_err_t err = esp_http_client_open(client, WAV_HEADER_BYTES + (int)data_size);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        ESP_LOGE(TAG, "reply open failed: %s", esp_err_to_name(err));
        return err;
    }

    if (esp_http_client_write(client, (const char *)&hdr, WAV_HEADER_BYTES) < 0) {
        err = ESP_FAIL;
        goto out;
    }

    /* Chunked so a 30-second reply does not sit in one giant TLS record. */
    const size_t chunk = 4096;
    size_t sent = 0;
    while (sent < data_size) {
        size_t n = (data_size - sent > chunk) ? chunk : data_size - sent;
        int written = esp_http_client_write(client, (const char *)pcm + sent, n);
        if (written < 0) {
            err = ESP_FAIL;
            goto out;
        }
        sent += (size_t)written;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 201 && status != 200) {
        ESP_LOGE(TAG, "reply -> HTTP %d", status);
        err = ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "reply uploaded: %u samples (%s ms)", (unsigned)samples, duration);
    }

out:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
