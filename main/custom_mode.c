#include "custom_mode.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"

#define CUSTOM_MODE_NAMESPACE "custom"

static char s_shortcuts[BUTTON_KEY_COUNT][CUSTOM_SHORTCUT_MAX_LEN + 1];
static custom_action_type_t s_action_types[BUTTON_KEY_COUNT];
static custom_http_method_t s_http_methods[BUTTON_KEY_COUNT];
static char s_http_urls[BUTTON_KEY_COUNT][CUSTOM_HTTP_URL_MAX_LEN + 1];
static char s_http_bodies[BUTTON_KEY_COUNT][CUSTOM_HTTP_BODY_MAX_LEN + 1];
static char s_wifi_ssid[CUSTOM_WIFI_SSID_MAX_LEN + 1];
static char s_wifi_password[CUSTOM_WIFI_PASSWORD_MAX_LEN + 1];

static const char *key_nvs_name(button_key_t key)
{
    switch (key) {
    case BUTTON_KEY_UP: return "up";
    case BUTTON_KEY_DOWN: return "down";
    case BUTTON_KEY_LEFT: return "left";
    case BUTTON_KEY_RIGHT: return "right";
    case BUTTON_KEY_FUNC1: return "f1";
    case BUTTON_KEY_FUNC2: return "f2";
    case BUTTON_KEY_FUNC3: return "f3";
    case BUTTON_KEY_FUNC4: return "f4";
    default: return NULL;
    }
}

static bool make_key_name(char *dst, size_t dst_len, const char *prefix, button_key_t key)
{
    const char *key_name = key_nvs_name(key);
    if (dst == NULL || prefix == NULL || key_name == NULL) {
        return false;
    }
    int written = snprintf(dst, dst_len, "%s_%s", prefix, key_name);
    return written > 0 && (size_t)written < dst_len;
}

static void copy_string(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strnlen(src, dst_len - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void copy_lower_token(char *dst, size_t dst_len, const char *src, size_t len)
{
    size_t n = len < dst_len - 1 ? len : dst_len - 1;
    for (size_t i = 0; i < n; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[n] = '\0';
}

static bool token_to_combo(const char *token, custom_key_combo_t *combo)
{
    if (token[0] != '\0' && token[1] == '\0') {
        char c = token[0];
        if (c >= 'A' && c <= 'Z') {
            combo->modifier = 0x02;
            combo->keycode = (uint8_t)(0x04 + c - 'A');
            return true;
        }
        if (c >= 'a' && c <= 'z') {
            combo->modifier = 0;
            combo->keycode = (uint8_t)(0x04 + c - 'a');
            return true;
        }
        if (c >= '1' && c <= '9') {
            combo->modifier = 0;
            combo->keycode = (uint8_t)(0x1E + c - '1');
            return true;
        }
        if (c == '0') {
            combo->modifier = 0;
            combo->keycode = 0x27;
            return true;
        }
    }

    char lower[16];
    copy_lower_token(lower, sizeof(lower), token, strlen(token));
    combo->modifier = 0;
    if (strcmp(lower, "enter") == 0) { combo->keycode = 0x28; return true; }
    if (strcmp(lower, "esc") == 0 || strcmp(lower, "escape") == 0) { combo->keycode = 0x29; return true; }
    if (strcmp(lower, "backspace") == 0) { combo->keycode = 0x2A; return true; }
    if (strcmp(lower, "tab") == 0) { combo->keycode = 0x2B; return true; }
    if (strcmp(lower, "space") == 0) { combo->keycode = 0x2C; return true; }
    if (strcmp(lower, "left") == 0) { combo->keycode = 0x50; return true; }
    if (strcmp(lower, "down") == 0) { combo->keycode = 0x51; return true; }
    if (strcmp(lower, "up") == 0) { combo->keycode = 0x52; return true; }
    if (strcmp(lower, "right") == 0) { combo->keycode = 0x4F; return true; }

    return false;
}

static bool parse_shortcut(const char *shortcut, custom_key_combo_t *combo)
{
    uint8_t modifier = 0;
    uint8_t keycode = 0;
    const char *part = shortcut;

    while (*part != '\0') {
        while (*part == ' ' || *part == '+') {
            part++;
        }
        const char *end = part;
        while (*end != '\0' && *end != '+') {
            end++;
        }

        while (end > part && end[-1] == ' ') {
            end--;
        }

        char token[16];
        copy_lower_token(token, sizeof(token), part, (size_t)(end - part));

        if (strcmp(token, "ctrl") == 0 || strcmp(token, "control") == 0) {
            modifier |= 0x01;
        } else if (strcmp(token, "shift") == 0) {
            modifier |= 0x02;
        } else if (strcmp(token, "alt") == 0 || strcmp(token, "option") == 0) {
            modifier |= 0x04;
        } else if (strcmp(token, "gui") == 0 || strcmp(token, "win") == 0 || strcmp(token, "cmd") == 0) {
            modifier |= 0x08;
        } else {
            char raw_token[16];
            custom_key_combo_t raw_combo;
            size_t raw_len = (size_t)(end - part);
            size_t n = raw_len < sizeof(raw_token) - 1 ? raw_len : sizeof(raw_token) - 1;
            memcpy(raw_token, part, n);
            raw_token[n] = '\0';
            if (!token_to_combo(raw_token, &raw_combo)) {
                return false;
            }
            modifier |= raw_combo.modifier;
            keycode = raw_combo.keycode;
        }

        part = *end == '+' ? end + 1 : end;
    }

    if (keycode == 0 && modifier == 0) {
        return false;
    }

    combo->modifier = modifier;
    combo->keycode = keycode;
    return true;
}

static bool append_combo(custom_key_combo_t *sequence, size_t max_count, size_t *count, custom_key_combo_t combo)
{
    if (*count >= max_count || (combo.modifier == 0 && combo.keycode == 0)) {
        return false;
    }
    sequence[*count] = combo;
    (*count)++;
    return true;
}

static bool parse_sequence_text(const char *shortcut, custom_key_combo_t *sequence, size_t max_count, size_t *count)
{
    const char *p = shortcut;
    *count = 0;

    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        const char *end = p;
        while (*end != '\0' && *end != ' ') {
            end++;
        }

        char token[16];
        size_t len = (size_t)(end - p);
        size_t n = len < sizeof(token) - 1 ? len : sizeof(token) - 1;
        memcpy(token, p, n);
        token[n] = '\0';

        custom_key_combo_t combo;
        if (strchr(token, '+') != NULL) {
            if (!parse_shortcut(token, &combo) || !append_combo(sequence, max_count, count, combo)) {
                return false;
            }
        } else if (token_to_combo(token, &combo)) {
            if (!append_combo(sequence, max_count, count, combo)) {
                return false;
            }
        } else if (len > 1) {
            for (size_t i = 0; i < len; i++) {
                char one[2] = {p[i], '\0'};
                if (!token_to_combo(one, &combo) || !append_combo(sequence, max_count, count, combo)) {
                    return false;
                }
            }
        } else {
            return false;
        }

        p = end;
    }

    return *count > 0;
}

esp_err_t custom_mode_init(void)
{
    memset(s_shortcuts, 0, sizeof(s_shortcuts));
    memset(s_action_types, 0, sizeof(s_action_types));
    memset(s_http_methods, 0, sizeof(s_http_methods));
    memset(s_http_urls, 0, sizeof(s_http_urls));
    memset(s_http_bodies, 0, sizeof(s_http_bodies));
    memset(s_wifi_ssid, 0, sizeof(s_wifi_ssid));
    memset(s_wifi_password, 0, sizeof(s_wifi_password));

    nvs_handle_t h;
    esp_err_t ret = nvs_open(CUSTOM_MODE_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        return ESP_OK;
    }

    size_t wifi_ssid_len = sizeof(s_wifi_ssid);
    size_t wifi_password_len = sizeof(s_wifi_password);
    nvs_get_str(h, "wifi_ssid", s_wifi_ssid, &wifi_ssid_len);
    nvs_get_str(h, "wifi_pass", s_wifi_password, &wifi_password_len);

    for (button_key_t key = BUTTON_KEY_UP; key < BUTTON_KEY_COUNT; key++) {
        const char *name = key_nvs_name(key);
        if (name == NULL) {
            continue;
        }
        size_t len = sizeof(s_shortcuts[key]);
        nvs_get_str(h, name, s_shortcuts[key], &len);

        char nvs_name[16];
        uint8_t value = 0;
        if (make_key_name(nvs_name, sizeof(nvs_name), "type", key) &&
            nvs_get_u8(h, nvs_name, &value) == ESP_OK &&
            value <= CUSTOM_ACTION_HTTP) {
            s_action_types[key] = (custom_action_type_t)value;
        }
        value = 0;
        if (make_key_name(nvs_name, sizeof(nvs_name), "method", key) &&
            nvs_get_u8(h, nvs_name, &value) == ESP_OK &&
            value <= CUSTOM_HTTP_POST) {
            s_http_methods[key] = (custom_http_method_t)value;
        }
        if (make_key_name(nvs_name, sizeof(nvs_name), "url", key)) {
            len = sizeof(s_http_urls[key]);
            nvs_get_str(h, nvs_name, s_http_urls[key], &len);
        }
        if (make_key_name(nvs_name, sizeof(nvs_name), "body", key)) {
            len = sizeof(s_http_bodies[key]);
            nvs_get_str(h, nvs_name, s_http_bodies[key], &len);
        }
    }

    nvs_close(h);
    return ESP_OK;
}

esp_err_t custom_mode_set_shortcut(button_key_t key, const char *shortcut)
{
    const char *name = key_nvs_name(key);
    if (name == NULL || shortcut == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strnlen(shortcut, CUSTOM_SHORTCUT_MAX_LEN);
    memcpy(s_shortcuts[key], shortcut, len);
    s_shortcuts[key][len] = '\0';

    nvs_handle_t h;
    esp_err_t ret = nvs_open(CUSTOM_MODE_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(h, name, s_shortcuts[key]);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

esp_err_t custom_mode_set_wifi(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    copy_string(s_wifi_ssid, sizeof(s_wifi_ssid), ssid);
    copy_string(s_wifi_password, sizeof(s_wifi_password), password);

    nvs_handle_t h;
    esp_err_t ret = nvs_open(CUSTOM_MODE_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(h, "wifi_ssid", s_wifi_ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(h, "wifi_pass", s_wifi_password);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

esp_err_t custom_mode_set_button_config(button_key_t key, const custom_button_config_t *config)
{
    const char *shortcut_name = key_nvs_name(key);
    if (shortcut_name == NULL || config == NULL ||
        config->type > CUSTOM_ACTION_HTTP || config->method > CUSTOM_HTTP_POST) {
        return ESP_ERR_INVALID_ARG;
    }

    copy_string(s_shortcuts[key], sizeof(s_shortcuts[key]), config->shortcut);
    copy_string(s_http_urls[key], sizeof(s_http_urls[key]), config->url);
    copy_string(s_http_bodies[key], sizeof(s_http_bodies[key]), config->body);
    s_action_types[key] = config->type;
    s_http_methods[key] = config->method;

    char type_name[16];
    char method_name[16];
    char url_name[16];
    char body_name[16];
    if (!make_key_name(type_name, sizeof(type_name), "type", key) ||
        !make_key_name(method_name, sizeof(method_name), "method", key) ||
        !make_key_name(url_name, sizeof(url_name), "url", key) ||
        !make_key_name(body_name, sizeof(body_name), "body", key)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t ret = nvs_open(CUSTOM_MODE_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(h, shortcut_name, s_shortcuts[key]);
    if (ret == ESP_OK) ret = nvs_set_u8(h, type_name, (uint8_t)s_action_types[key]);
    if (ret == ESP_OK) ret = nvs_set_u8(h, method_name, (uint8_t)s_http_methods[key]);
    if (ret == ESP_OK) ret = nvs_set_str(h, url_name, s_http_urls[key]);
    if (ret == ESP_OK) ret = nvs_set_str(h, body_name, s_http_bodies[key]);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

esp_err_t custom_mode_clear_all(void)
{
    memset(s_shortcuts, 0, sizeof(s_shortcuts));
    memset(s_action_types, 0, sizeof(s_action_types));
    memset(s_http_methods, 0, sizeof(s_http_methods));
    memset(s_http_urls, 0, sizeof(s_http_urls));
    memset(s_http_bodies, 0, sizeof(s_http_bodies));
    memset(s_wifi_ssid, 0, sizeof(s_wifi_ssid));
    memset(s_wifi_password, 0, sizeof(s_wifi_password));

    nvs_handle_t h;
    esp_err_t ret = nvs_open(CUSTOM_MODE_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
    }

    ret = nvs_erase_all(h);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

const char *custom_mode_get_shortcut(button_key_t key)
{
    if (key < 0 || key >= BUTTON_KEY_COUNT) {
        return "";
    }
    return s_shortcuts[key];
}

const char *custom_mode_get_wifi_ssid(void)
{
    return s_wifi_ssid;
}

const char *custom_mode_get_wifi_password(void)
{
    return s_wifi_password;
}

custom_action_type_t custom_mode_get_action_type(button_key_t key)
{
    if (key < 0 || key >= BUTTON_KEY_COUNT) {
        return CUSTOM_ACTION_SHORTCUT;
    }
    return s_action_types[key];
}

bool custom_mode_get_button_config(button_key_t key, custom_button_config_t *config)
{
    if (key < 0 || key >= BUTTON_KEY_COUNT || config == NULL) {
        return false;
    }
    memset(config, 0, sizeof(*config));
    config->type = s_action_types[key];
    config->method = s_http_methods[key];
    copy_string(config->shortcut, sizeof(config->shortcut), s_shortcuts[key]);
    copy_string(config->url, sizeof(config->url), s_http_urls[key]);
    copy_string(config->body, sizeof(config->body), s_http_bodies[key]);
    return true;
}

bool custom_mode_get_combo(button_key_t key, custom_key_combo_t *combo)
{
    if (key < 0 || key >= BUTTON_KEY_COUNT || combo == NULL || s_shortcuts[key][0] == '\0') {
        return false;
    }

    return parse_shortcut(s_shortcuts[key], combo);
}

bool custom_mode_get_sequence(button_key_t key, custom_key_combo_t *sequence, size_t max_count, size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (key < 0 || key >= BUTTON_KEY_COUNT || sequence == NULL || count == NULL ||
        max_count == 0 || s_shortcuts[key][0] == '\0') {
        return false;
    }

    return parse_sequence_text(s_shortcuts[key], sequence, max_count, count);
}
