#include "device_names.h"
#include "web_control.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "custom_mode.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define WEB_AP_SSID "自定义设置"
#define WEB_AP_PASSWORD ""
#define WEB_AP_CHANNEL 6
#define DNS_PORT 53
#define WEB_SAVE_BODY_MAX 24576
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define CUSTOM_HTTP_CONNECT_TIMEOUT_MS 15000
#define CUSTOM_HTTP_REQUEST_TIMEOUT_MS 12000
#define CUSTOM_HTTP_RESULT_DISPLAY_MS 2500
#define CUSTOM_HTTP_TASK_STACK 8192
#define WEB_CONTROL_TASK_STACK 8192
#define WEB_WIFI_SCAN_MAX 32
#define CUSTOM_HTTP_ENCODED_URL_MAX_LEN (CUSTOM_HTTP_URL_MAX_LEN * 3 + 1)

static const char *TAG = "web_control";

static bool s_netif_ready;
static bool s_wifi_started;
static bool s_dns_running;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;
static TaskHandle_t s_custom_http_task;
static TaskHandle_t s_web_control_task;
static volatile bool s_web_control_requested;
static int s_dns_sock = -1;
static EventGroupHandle_t s_wifi_events;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static portMUX_TYPE s_http_status_lock = portMUX_INITIALIZER_UNLOCKED;
static custom_http_status_t s_custom_http_status;
static char s_wifi_scan_ssids[WEB_WIFI_SCAN_MAX][CUSTOM_WIFI_SSID_MAX_LEN + 1];
static uint16_t s_wifi_scan_count;

typedef struct {
    char ssid[CUSTOM_WIFI_SSID_MAX_LEN + 1];
    char password[CUSTOM_WIFI_PASSWORD_MAX_LEN + 1];
    custom_http_method_t method;
    char url[CUSTOM_HTTP_URL_MAX_LEN + 1];
    char body[CUSTOM_HTTP_BODY_MAX_LEN + 1];
} custom_http_task_config_t;

static custom_http_task_config_t s_custom_http_config;

static const button_key_t WEB_KEYS[] = {
    BUTTON_KEY_UP, BUTTON_KEY_DOWN, BUTTON_KEY_LEFT, BUTTON_KEY_RIGHT,
    BUTTON_KEY_FUNC2, BUTTON_KEY_FUNC3, BUTTON_KEY_FUNC4,
};

static const char * const WEB_KEY_IDS[] = {
    "up", "down", "left", "right", "f2", "f3", "f4",
};

static const char * const WEB_KEY_LABELS[] = {
    "上", "下", "左", "右", "功能二", "功能三", "功能四",
};

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_range(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t used = 0;
    size_t pos = 0;
    while (pos < src_len && used + 1 < dst_len) {
        if (src[pos] == '+') {
            dst[used++] = ' ';
            pos++;
        } else if (src[pos] == '%' && pos + 2 < src_len &&
                   isxdigit((unsigned char)src[pos + 1]) &&
                   isxdigit((unsigned char)src[pos + 2])) {
            dst[used++] = (char)((hex_value(src[pos + 1]) << 4) | hex_value(src[pos + 2]));
            pos += 3;
        } else {
            dst[used++] = src[pos++];
        }
    }
    dst[used] = '\0';
}

static bool find_param(const char *body, const char *name, char *value, size_t value_len)
{
    size_t name_len = strlen(name);
    const char *p = body;
    while (p != NULL && *p != '\0') {
        if (strncmp(p, name, name_len) == 0 && p[name_len] == '=') {
            const char *start = p + name_len + 1;
            const char *end = strchr(start, '&');
            size_t len = end != NULL ? (size_t)(end - start) : strlen(start);
            url_decode_range(value, value_len, start, len);
            return true;
        }
        p = strchr(p, '&');
        if (p != NULL) {
            p++;
        }
    }
    value[0] = '\0';
    return false;
}

static void html_attr_escape(char *dst, size_t dst_len, const char *src)
{
    size_t used = 0;
    while (*src != '\0' && used + 1 < dst_len) {
        const char *rep = NULL;
        if (*src == '&') rep = "&amp;";
        else if (*src == '"') rep = "&quot;";
        else if (*src == 39) rep = "&#39;";
        else if (*src == '<') rep = "&lt;";
        else if (*src == '>') rep = "&gt;";

        if (rep != NULL) {
            size_t rep_len = strlen(rep);
            if (used + rep_len >= dst_len) {
                break;
            }
            memcpy(dst + used, rep, rep_len);
            used += rep_len;
        } else {
            dst[used++] = *src;
        }
        src++;
    }
    dst[used] = '\0';
}

static void set_custom_http_status(custom_http_state_t state, int http_status, const char *detail)
{
    portENTER_CRITICAL(&s_http_status_lock);
    s_custom_http_status.state = state;
    s_custom_http_status.http_status = http_status;
    snprintf(s_custom_http_status.detail, sizeof(s_custom_http_status.detail), "%s",
             detail != NULL ? detail : "");
    portEXIT_CRITICAL(&s_http_status_lock);
}

void web_control_get_custom_http_status(custom_http_status_t *status)
{
    if (status == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_http_status_lock);
    *status = s_custom_http_status;
    portEXIT_CRITICAL(&s_http_status_lock);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (s_wifi_events == NULL) {
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_events, WIFI_DISCONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t ensure_wifi_ready(void)
{
    if (s_netif_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    esp_err_t event_ret = esp_event_loop_create_default();
    if (event_ret != ESP_OK && event_ret != ESP_ERR_INVALID_STATE) {
        return event_ret;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_ap_netif == NULL || s_sta_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                            wifi_event_handler, NULL, &s_wifi_event_instance),
        TAG, "register wifi event failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            wifi_event_handler, NULL, &s_ip_event_instance),
        TAG, "register ip event failed");

    s_netif_ready = true;
    return ESP_OK;
}

static esp_err_t refresh_wifi_scan_cache(void)
{
    s_wifi_scan_count = 0;
    memset(s_wifi_scan_ssids, 0, sizeof(s_wifi_scan_ssids));

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set scan storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set scan mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start scan wifi failed");

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        esp_wifi_stop();
        return ret;
    }

    uint16_t count = 0;
    ret = esp_wifi_scan_get_ap_num(&count);
    if (ret != ESP_OK) {
        esp_wifi_stop();
        return ret;
    }
    if (count > WEB_WIFI_SCAN_MAX) {
        count = WEB_WIFI_SCAN_MAX;
    }

    wifi_ap_record_t *records = count > 0 ? calloc(count, sizeof(*records)) : NULL;
    if (count > 0 && records == NULL) {
        esp_wifi_stop();
        return ESP_ERR_NO_MEM;
    }
    if (count > 0) {
        ret = esp_wifi_scan_get_ap_records(&count, records);
    }
    if (ret == ESP_OK) {
        for (uint16_t i = 0; i < count && s_wifi_scan_count < WEB_WIFI_SCAN_MAX; i++) {
            const char *ssid = (const char *)records[i].ssid;
            if (ssid[0] == '\0') {
                continue;
            }
            bool duplicate = false;
            for (uint16_t j = 0; j < s_wifi_scan_count; j++) {
                if (strcmp(ssid, s_wifi_scan_ssids[j]) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                snprintf(s_wifi_scan_ssids[s_wifi_scan_count],
                         sizeof(s_wifi_scan_ssids[s_wifi_scan_count]), "%s", ssid);
                s_wifi_scan_count++;
            }
        }
    }

    free(records);
    esp_wifi_stop();
    ESP_LOGI(TAG, "Wi-Fi scan cached %u networks", s_wifi_scan_count);
    return ret;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
        "<title>万能遥控器</title><style>"
        "body{margin:0;background:#111;color:#eee;font-family:system-ui,-apple-system,sans-serif}"
        "main{max-width:620px;margin:0 auto;padding:18px 18px 82px}h1{font-size:22px;margin:0 0 14px}"
        "h2{font-size:17px;margin:22px 0 10px}.card{border:1px solid #333;border-radius:12px;padding:12px;margin:12px 0;background:#181818}"
        ".row{display:flex;align-items:center;gap:10px;margin:9px 0}.row label{width:72px;flex:none}"
        "input,select,textarea{box-sizing:border-box;flex:1;min-width:0;padding:11px;border-radius:8px;border:1px solid #444;background:#242424;color:#fff;font-size:15px}"
        "textarea{min-height:72px;resize:vertical}.primary,.scan{border:0;border-radius:8px;background:#2e7dff;color:white;font-size:16px}"
        ".primary{width:100%;padding:14px;margin-top:16px}.scan{width:auto;padding:11px 13px;margin:0}"
        ".hint{color:#aaa;font-size:13px;line-height:1.5}.hidden{display:none}"
        ".tab-page{display:none}.tab-page.active{display:block}.tabbar{position:fixed;left:0;right:0;bottom:0;display:flex;"
        "gap:8px;padding:10px max(12px,env(safe-area-inset-left));background:#181818;border-top:1px solid #333;z-index:10}"
        ".tabbar button{flex:1;padding:12px;border:0;border-radius:9px;background:#292929;color:#aaa;font-size:16px}"
        ".tabbar button.active{background:#2e7dff;color:#fff}</style></head><body><main>"
        "<h1>自定义模式</h1><section id='tab_wifi' class='tab-page active'>"
        "<form method='post' action='/save_wifi'>"
        "<h2>联网设置</h2><div class='card'>");

    char wifi_values[1800];
    char escaped_ssid[(CUSTOM_WIFI_SSID_MAX_LEN + 1) * 5];
    char escaped_password[(CUSTOM_WIFI_PASSWORD_MAX_LEN + 1) * 5];
    html_attr_escape(escaped_ssid, sizeof(escaped_ssid), custom_mode_get_wifi_ssid());
    html_attr_escape(escaped_password, sizeof(escaped_password), custom_mode_get_wifi_password());
    snprintf(wifi_values, sizeof(wifi_values),
             "<div class='row'><label>Wi-Fi</label><select id='wifi_ssid' name='wifi_ssid'>"
             "<option value='%s'>%s</option></select>"
             "<button class='scan' type='button' onclick='scanWifi()'>扫描</button></div>"
             "<div class='row'><label>密码</label><input type='text' name='wifi_pass' maxlength='%d' "
             "value='%s' placeholder='开放网络可留空'></div>"
             "<div class='row'><label>开放网络</label><input style='flex:none;width:22px;height:22px' "
             "type='checkbox' name='wifi_clear' value='1'><span class='hint'>清除已保存密码</span></div>"
             "<div id='scan_status' class='hint'>网络已在热点启动前扫描；重新进入网页控制可刷新列表。</div></div>"
             "<button class='primary' type='submit'>保存 Wi-Fi 设置</button></form>",
             escaped_ssid, escaped_ssid[0] != '\0' ? escaped_ssid : "请先扫描 Wi-Fi",
             CUSTOM_WIFI_PASSWORD_MAX_LEN, escaped_password);
    httpd_resp_sendstr_chunk(req, wifi_values);
    httpd_resp_sendstr_chunk(req,
        "<h2>设备名称</h2><form method='post' action='/save_devices'><div class='card'>"
        "<p class='hint'>名称显示在遥控器的当前设备和设备列表中。支持中文，最多16个字；留空恢复默认名称。保存后断电保留。</p>");
    for (uint8_t i = 0; i < DEVICE_NAME_COUNT; ++i) {
        char name[DEVICE_NAME_BYTES + 1], escaped[(DEVICE_NAME_BYTES + 1) * 6], field[600];
        device_names_get(i, name, sizeof(name));
        html_attr_escape(escaped, sizeof(escaped), name);
        snprintf(field, sizeof(field),
                 "<div class='row'><label>设备%u</label><input name='device%u' maxlength='%u' value='%s' placeholder='设备%u'></div>",
                 i + 1, i + 1, DEVICE_NAME_CHARS, escaped, i + 1);
        httpd_resp_sendstr_chunk(req, field);
    }
    httpd_resp_sendstr_chunk(req,
        "</div><button class='primary' type='submit'>保存设备名称</button></form></section>"
        "<section id='tab_buttons' class='tab-page'><form method='post' action='/save_buttons'><h2>按键功能</h2>");

    char *row = calloc(7000, 1);
    char *escaped_values = calloc(5200, 1);
    if (row == NULL || escaped_values == NULL) {
        free(row);
        free(escaped_values);
        httpd_resp_sendstr_chunk(req, "<p>内存不足，无法生成配置页面。</p></main></body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_ERR_NO_MEM;
    }
    char *shortcut = escaped_values;
    char *url = shortcut + (CUSTOM_SHORTCUT_MAX_LEN + 1) * 5;
    char *body = url + (CUSTOM_HTTP_URL_MAX_LEN + 1) * 5;
    for (size_t i = 0; i < sizeof(WEB_KEYS) / sizeof(WEB_KEYS[0]); i++) {
        custom_button_config_t config;
        custom_mode_get_button_config(WEB_KEYS[i], &config);
        html_attr_escape(shortcut, (CUSTOM_SHORTCUT_MAX_LEN + 1) * 5, config.shortcut);
        html_attr_escape(url, (CUSTOM_HTTP_URL_MAX_LEN + 1) * 5, config.url);
        html_attr_escape(body, (CUSTOM_HTTP_BODY_MAX_LEN + 1) * 5, config.body);
        snprintf(row, 7000,
                 "<div class='card key-card'><div class='row'><label>%s</label>"
                 "<select name='type_%s' onchange='toggleCard(this)'>"
                 "<option value='shortcut'%s>键盘快捷键</option><option value='http'%s>HTTP 请求</option></select></div>"
                 "<div class='shortcut-fields%s'><div class='row'><label>快捷键</label>"
                 "<input name='%s' maxlength='%d' value='%s' placeholder='ctrl+c'></div></div>"
                 "<div class='http-fields%s'><div class='row'><label>方法</label><select name='method_%s'>"
                 "<option value='get'%s>GET</option><option value='post'%s>POST</option></select></div>"
                 "<div class='row'><label>URL</label><textarea name='url_%s' maxlength='%d' rows='3' "
                 "placeholder='https://example.com/api'>%s</textarea></div>"
                 "<div class='row'><label>POST 数据</label><textarea name='body_%s' maxlength='%d' "
                 "placeholder='{\"key\":\"value\"}'>%s</textarea></div></div></div>",
                 WEB_KEY_LABELS[i], WEB_KEY_IDS[i],
                 config.type == CUSTOM_ACTION_SHORTCUT ? " selected" : "",
                 config.type == CUSTOM_ACTION_HTTP ? " selected" : "",
                 config.type == CUSTOM_ACTION_SHORTCUT ? "" : " hidden",
                 WEB_KEY_IDS[i], CUSTOM_SHORTCUT_MAX_LEN, shortcut,
                 config.type == CUSTOM_ACTION_HTTP ? "" : " hidden",
                 WEB_KEY_IDS[i],
                 config.method == CUSTOM_HTTP_GET ? " selected" : "",
                 config.method == CUSTOM_HTTP_POST ? " selected" : "",
                 WEB_KEY_IDS[i], CUSTOM_HTTP_URL_MAX_LEN, url,
                 WEB_KEY_IDS[i], CUSTOM_HTTP_BODY_MAX_LEN, body);
        httpd_resp_sendstr_chunk(req, row);
    }
    free(row);
    free(escaped_values);

    httpd_resp_sendstr_chunk(req,
        "<button class='primary' type='submit'>保存按键设置</button></form>"
        "<p class='hint'>快捷键示例：ctrl+c、shift+a、enter、H e l l o。HTTP 按键会连接上方 Wi-Fi，"
        "发送一次 GET 或 POST 后自动断开；POST 数据按 application/json 发送。支持 http:// 和 https://。</p></section>"
        "</main><nav class='tabbar'><button id='nav_wifi' type='button' onclick=\"showTab('wifi')\">Wi-Fi 设置</button>"
        "<button id='nav_buttons' type='button' onclick=\"showTab('buttons')\">按键设置</button></nav>"
        "<script>function toggleCard(s){const c=s.closest('.key-card'),h=s.value==='http';"
        "c.querySelector('.shortcut-fields').classList.toggle('hidden',h);"
        "c.querySelector('.http-fields').classList.toggle('hidden',!h)}"
        "function showTab(n){['wifi','buttons'].forEach(x=>{document.getElementById('tab_'+x).classList.toggle('active',x===n);"
        "document.getElementById('nav_'+x).classList.toggle('active',x===n)});history.replaceState(null,'','/?tab='+n)}"
        "async function scanWifi(){const e=document.getElementById('scan_status'),s=document.getElementById('wifi_ssid');"
        "e.textContent='正在扫描，请稍候…';try{const r=await fetch('/scan');if(!r.ok)throw Error();"
        "const a=await r.json(),old=s.value;s.innerHTML='';"
        "if(old){s.add(new Option(old,old));}else{s.add(new Option('请选择 Wi-Fi',''));}"
        "a.forEach(x=>{if(x!==old)s.add(new Option(x,x))});"
        "e.textContent=a.length?'找到 '+a.length+' 个网络，请从下拉框选择':'没有发现网络，请重新扫描';}"
        "catch(x){e.textContent='扫描失败，请重试';}}"
        "showTab(new URLSearchParams(location.search).get('tab')==='buttons'?'buttons':'wifi');</script></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void json_escape(char *dst, size_t dst_len, const char *src)
{
    size_t used = 0;
    while (*src != '\0' && used + 1 < dst_len) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\') {
            if (used + 2 >= dst_len) break;
            dst[used++] = '\\';
            dst[used++] = (char)c;
        } else if (c >= 0x20) {
            dst[used++] = (char)c;
        }
    }
    dst[used] = '\0';
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    for (uint16_t i = 0; i < s_wifi_scan_count; i++) {
        char escaped[2 * (CUSTOM_WIFI_SSID_MAX_LEN + 1)];
        char item[2 * (CUSTOM_WIFI_SSID_MAX_LEN + 1) + 4];
        json_escape(escaped, sizeof(escaped), s_wifi_scan_ssids[i]);
        snprintf(item, sizeof(item), "%s\"%s\"", first ? "" : ",", escaped);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t receive_post_body(httpd_req_t *req, char **body_out)
{
    if (body_out == NULL || req->content_len <= 0 || req->content_len >= WEB_SAVE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Save data is too large");
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = calloc((size_t)req->content_len + 1, 1);
    if (body == NULL) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    int remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, body + offset, remaining);
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        offset += ret;
        remaining -= ret;
    }
    body[offset] = '\0';
    *body_out = body;
    return ESP_OK;
}

static esp_err_t send_save_redirect(httpd_req_t *req, const char *tab)
{
    char location[32];
    snprintf(location, sizeof(location), "/?tab=%s", tab);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t save_devices_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t ret = receive_post_body(req, &body);
    if (ret != ESP_OK) return ret;
    char names[DEVICE_NAME_COUNT][DEVICE_NAME_BYTES + 1] = {{0}};
    for (uint8_t i = 0; i < DEVICE_NAME_COUNT; ++i) {
        char key[12], value[DEVICE_NAME_BYTES + 2];
        snprintf(key, sizeof(key), "device%u", i + 1);
        if (!find_param(body, key, value, sizeof(value)) || strlen(value) > DEVICE_NAME_BYTES) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device names are missing or too long");
        }
        memcpy(names[i], value, strlen(value) + 1);
    }
    free(body);
    ret = device_names_save(names);
    if (ret == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use up to 16 characters per name without control characters");
    if (ret != ESP_OK) return httpd_resp_send_500(req);
    ESP_LOGI(TAG, "device names saved");
    return send_save_redirect(req, "wifi");
}

static esp_err_t save_wifi_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t ret = receive_post_body(req, &body);
    if (ret != ESP_OK) {
        return ret;
    }

    char wifi_ssid[CUSTOM_WIFI_SSID_MAX_LEN + 1];
    char wifi_password[CUSTOM_WIFI_PASSWORD_MAX_LEN + 1];
    char wifi_clear[4];
    find_param(body, "wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    find_param(body, "wifi_pass", wifi_password, sizeof(wifi_password));
    bool clear_password = find_param(body, "wifi_clear", wifi_clear, sizeof(wifi_clear));
    if (wifi_password[0] == '\0' && !clear_password) {
        snprintf(wifi_password, sizeof(wifi_password), "%s", custom_mode_get_wifi_password());
    }
    ret = custom_mode_set_wifi(wifi_ssid, wifi_password);
    free(body);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "save Wi-Fi configuration failed: %s", esp_err_to_name(ret));
        httpd_resp_send_500(req);
        return ret;
    }
    ESP_LOGI(TAG, "Wi-Fi configuration saved: ssid=%s", wifi_ssid);
    return send_save_redirect(req, "wifi");
}

static esp_err_t save_buttons_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t ret = receive_post_body(req, &body);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_err_t save_ret = ESP_OK;
    for (size_t i = 0; i < sizeof(WEB_KEYS) / sizeof(WEB_KEYS[0]); i++) {
        custom_button_config_t config = {0};
        char param_name[24];
        char value[16];

        snprintf(param_name, sizeof(param_name), "type_%s", WEB_KEY_IDS[i]);
        find_param(body, param_name, value, sizeof(value));
        config.type = strcmp(value, "http") == 0 ? CUSTOM_ACTION_HTTP : CUSTOM_ACTION_SHORTCUT;

        snprintf(param_name, sizeof(param_name), "method_%s", WEB_KEY_IDS[i]);
        find_param(body, param_name, value, sizeof(value));
        config.method = strcmp(value, "post") == 0 ? CUSTOM_HTTP_POST : CUSTOM_HTTP_GET;

        find_param(body, WEB_KEY_IDS[i], config.shortcut, sizeof(config.shortcut));
        snprintf(param_name, sizeof(param_name), "url_%s", WEB_KEY_IDS[i]);
        find_param(body, param_name, config.url, sizeof(config.url));
        snprintf(param_name, sizeof(param_name), "body_%s", WEB_KEY_IDS[i]);
        find_param(body, param_name, config.body, sizeof(config.body));

        if (save_ret == ESP_OK) {
            save_ret = custom_mode_set_button_config(WEB_KEYS[i], &config);
        }
    }
    free(body);

    if (save_ret != ESP_OK) {
        ESP_LOGE(TAG, "save custom configuration failed: %s", esp_err_to_name(save_ret));
        httpd_resp_send_500(req);
        return save_ret;
    }

    ESP_LOGI(TAG, "button configuration saved");
    return send_save_redirect(req, "buttons");
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_dns_sock = sock;

    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = 100000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&server, sizeof(server));

    uint8_t packet[512];
    while (s_dns_running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, packet, sizeof(packet), 0, (struct sockaddr *)&from, &from_len);
        if (!s_dns_running) {
            break;
        }
        if (len < 12) {
            continue;
        }

        uint8_t response[512];
        memcpy(response, packet, len);
        response[2] = 0x81;
        response[3] = 0x80;
        response[7] = 0x01;
        int pos = len;
        if (pos + 16 <= (int)sizeof(response)) {
            response[pos++] = 0xC0; response[pos++] = 0x0C;
            response[pos++] = 0x00; response[pos++] = 0x01;
            response[pos++] = 0x00; response[pos++] = 0x01;
            response[pos++] = 0x00; response[pos++] = 0x00; response[pos++] = 0x00; response[pos++] = 0x3C;
            response[pos++] = 0x00; response[pos++] = 0x04;
            response[pos++] = 192; response[pos++] = 168; response[pos++] = 4; response[pos++] = 1;
            sendto(sock, response, pos, 0, (struct sockaddr *)&from, from_len);
        }
    }

    closesocket(sock);
    s_dns_sock = -1;
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static bool encode_http_url(char *dst, size_t dst_len, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    while (*src != '\0') {
        unsigned char c = (unsigned char)*src++;
        bool keep = isalnum(c) || strchr("-._~:/?#[]@!$&'()*+,;=%", c) != NULL;
        if (keep) {
            if (used + 1 >= dst_len) {
                return false;
            }
            dst[used++] = (char)c;
        } else {
            if (used + 3 >= dst_len) {
                return false;
            }
            dst[used++] = '%';
            dst[used++] = hex[c >> 4];
            dst[used++] = hex[c & 0x0F];
        }
    }
    dst[used] = '\0';
    return true;
}

static void custom_http_task(void *arg)
{
    (void)arg;
    custom_http_task_config_t config = s_custom_http_config;
    bool wifi_running = false;
    bool request_ok = false;
    int http_status = 0;
    char result_detail[64] = "";

    if (config.ssid[0] == '\0') {
        snprintf(result_detail, sizeof(result_detail), "未配置 WiFi");
        goto finish;
    }
    if (config.url[0] == '\0') {
        snprintf(result_detail, sizeof(result_detail), "未配置 URL");
        goto finish;
    }
    if (strncmp(config.url, "http://", 7) != 0 &&
        strncmp(config.url, "https://", 8) != 0) {
        snprintf(result_detail, sizeof(result_detail), "URL 格式错误");
        goto finish;
    }

    set_custom_http_status(CUSTOM_HTTP_STATE_STARTING_WIFI, 0, "初始化网络");
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_err_t ret = ensure_wifi_ready();
    if (ret != ESP_OK) {
        snprintf(result_detail, sizeof(result_detail), "WiFi初始化失败");
        goto finish;
    }

    wifi_config_t wifi_config = {0};
    size_t ssid_len = strnlen(config.ssid, CUSTOM_WIFI_SSID_MAX_LEN);
    size_t password_len = strnlen(config.password, CUSTOM_WIFI_PASSWORD_MAX_LEN);
    memcpy(wifi_config.sta.ssid, config.ssid,
           ssid_len < sizeof(wifi_config.sta.ssid) ? ssid_len : sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.password, config.password,
           password_len < sizeof(wifi_config.sta.password) ? password_len : sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret == ESP_OK) ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret == ESP_OK) ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_OK) ret = esp_wifi_start();
    if (ret != ESP_OK) {
        snprintf(result_detail, sizeof(result_detail), "WiFi启动失败");
        goto finish;
    }
    s_wifi_started = true;
    wifi_running = true;

    set_custom_http_status(CUSTOM_HTTP_STATE_CONNECTING_WIFI, 0, config.ssid);
    vTaskDelay(pdMS_TO_TICKS(120));
    bool connected = false;
    for (int attempt = 0; attempt < 3 && !connected; attempt++) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT);
        ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            continue;
        }
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_events, WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(CUSTOM_HTTP_CONNECT_TIMEOUT_MS / 3));
        connected = (bits & WIFI_CONNECTED_BIT) != 0;
    }
    if (!connected) {
        snprintf(result_detail, sizeof(result_detail), "连接失败/超时");
        goto disconnect;
    }

    set_custom_http_status(CUSTOM_HTTP_STATE_SENDING, 0,
                           config.method == CUSTOM_HTTP_POST ? "正在发送 POST" : "正在发送 GET");
    vTaskDelay(pdMS_TO_TICKS(120));
    char encoded_url[CUSTOM_HTTP_ENCODED_URL_MAX_LEN];
    if (!encode_http_url(encoded_url, sizeof(encoded_url), config.url)) {
        snprintf(result_detail, sizeof(result_detail), "URL 太长");
        goto disconnect;
    }
    esp_http_client_config_t http_config = {
        .url = encoded_url,
        .timeout_ms = CUSTOM_HTTP_REQUEST_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (client == NULL) {
        snprintf(result_detail, sizeof(result_detail), "请求初始化失败");
        goto disconnect;
    }

    if (config.method == CUSTOM_HTTP_POST) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, config.body, (int)strlen(config.body));
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }

    ret = esp_http_client_perform(client);
    if (ret == ESP_OK) {
        http_status = esp_http_client_get_status_code(client);
        request_ok = http_status >= 200 && http_status < 400;
        snprintf(result_detail, sizeof(result_detail), "HTTP %d", http_status);
    } else {
        snprintf(result_detail, sizeof(result_detail), "%s", esp_err_to_name(ret));
    }
    esp_http_client_cleanup(client);

disconnect:
    set_custom_http_status(CUSTOM_HTTP_STATE_DISCONNECTING, http_status, "关闭 WiFi");
    vTaskDelay(pdMS_TO_TICKS(120));
    if (wifi_running) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
        wifi_running = false;
    }

finish:
    if (wifi_running) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }
    set_custom_http_status(request_ok ? CUSTOM_HTTP_STATE_SUCCESS : CUSTOM_HTTP_STATE_FAILED,
                           http_status, result_detail[0] != '\0' ? result_detail : "请求失败");
    vTaskDelay(pdMS_TO_TICKS(CUSTOM_HTTP_RESULT_DISPLAY_MS));
    set_custom_http_status(CUSTOM_HTTP_STATE_IDLE, http_status, "");
    s_custom_http_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t web_control_start_custom_http(button_key_t key)
{
    if (s_custom_http_task != NULL || s_wifi_started) {
        return ESP_ERR_INVALID_STATE;
    }

    custom_button_config_t button_config;
    if (!custom_mode_get_button_config(key, &button_config) ||
        button_config.type != CUSTOM_ACTION_HTTP) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_custom_http_config, 0, sizeof(s_custom_http_config));
    snprintf(s_custom_http_config.ssid, sizeof(s_custom_http_config.ssid), "%s",
             custom_mode_get_wifi_ssid());
    snprintf(s_custom_http_config.password, sizeof(s_custom_http_config.password), "%s",
             custom_mode_get_wifi_password());
    s_custom_http_config.method = button_config.method;
    snprintf(s_custom_http_config.url, sizeof(s_custom_http_config.url), "%s", button_config.url);
    snprintf(s_custom_http_config.body, sizeof(s_custom_http_config.body), "%s", button_config.body);

    set_custom_http_status(CUSTOM_HTTP_STATE_STARTING_WIFI, 0, "准备执行");
    BaseType_t task_ret = xTaskCreate(custom_http_task, "custom_http", CUSTOM_HTTP_TASK_STACK,
                                      NULL, 5, &s_custom_http_task);
    if (task_ret != pdPASS) {
        s_custom_http_task = NULL;
        set_custom_http_status(CUSTOM_HTTP_STATE_IDLE, 0, "");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t web_control_start_sync(void)
{
    if (s_wifi_started) {
        return ESP_OK;
    }
    if (s_custom_http_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(ensure_wifi_ready(), TAG, "prepare wifi failed");
    esp_err_t scan_ret = refresh_wifi_scan_cache();
    if (scan_ret != ESP_OK) {
        ESP_LOGW(TAG, "pre-AP Wi-Fi scan failed: %s", esp_err_to_name(scan_ret));
    }

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), "%s", WEB_AP_SSID);
    wifi_config.ap.ssid_len = strlen(WEB_AP_SSID);
    wifi_config.ap.channel = WEB_AP_CHANNEL;
    wifi_config.ap.max_connection = 2;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set apsta mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "ap config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    s_wifi_started = true;

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;
    http_cfg.stack_size = 8192;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &http_cfg), TAG, "http start failed");

    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_uri_t scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get_handler};
    httpd_uri_t save_wifi = {
        .uri = "/save_wifi", .method = HTTP_POST, .handler = save_wifi_post_handler
    };
    httpd_uri_t save_buttons = {
        .uri = "/save_buttons", .method = HTTP_POST, .handler = save_buttons_post_handler
    };
    httpd_uri_t all = {.uri = "/*", .method = HTTP_GET, .handler = redirect_handler};
    httpd_uri_t save_devices = {.uri = "/save_devices", .method = HTTP_POST, .handler = save_devices_post_handler};
    httpd_register_uri_handler(s_httpd, &save_devices);
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &save_wifi);
    httpd_register_uri_handler(s_httpd, &save_buttons);
    httpd_register_uri_handler(s_httpd, &all);

    s_dns_running = true;
    xTaskCreate(dns_task, "dns_captive", 4096, NULL, 5, &s_dns_task);

    ESP_LOGI(TAG, "web control started: ssid=%s url=http://192.168.4.1", WEB_AP_SSID);
    return ESP_OK;
}

static esp_err_t web_control_stop_sync(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    s_dns_running = false;
    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }

    ESP_LOGI(TAG, "web control stopped");
    return ESP_OK;
}

static void web_control_task(void *arg)
{
    (void)arg;
    if (s_web_control_requested) {
        esp_err_t ret = web_control_start_sync();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "web control start failed: %s", esp_err_to_name(ret));
            s_web_control_requested = false;
        }
        if (!s_web_control_requested && s_wifi_started) {
            web_control_stop_sync();
        }
    } else if (s_wifi_started) {
        web_control_stop_sync();
    }
    s_web_control_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t schedule_web_control_task(void)
{
    if (s_web_control_task != NULL) {
        return ESP_OK;
    }
    BaseType_t task_ret = xTaskCreate(web_control_task, "web_control",
                                      WEB_CONTROL_TASK_STACK, NULL, 5,
                                      &s_web_control_task);
    if (task_ret != pdPASS) {
        s_web_control_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t web_control_start(void)
{
    if (s_custom_http_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_web_control_requested = true;
    return schedule_web_control_task();
}

esp_err_t web_control_stop(void)
{
    s_web_control_requested = false;
    return schedule_web_control_task();
}

bool web_control_is_running(void)
{
    return s_wifi_started;
}
