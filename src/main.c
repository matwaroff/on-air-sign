#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef ON_AIR_AP_PASSWORD
#define ON_AIR_AP_PASSWORD "onair1234"
#endif

#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME "onair"
#endif

#ifndef CONTROLLER_PRESS_GPIO
#define CONTROLLER_PRESS_GPIO 21
#endif

#ifndef SWITCH_GPIO
#define SWITCH_GPIO 2
#endif

#ifndef CONTROLLER_PRESS_ACTIVE_HIGH
#define CONTROLLER_PRESS_ACTIVE_HIGH 1
#endif

#ifndef CONTROLLER_PRESS_MS
#define CONTROLLER_PRESS_MS 250
#endif

#ifndef CONTROLLER_GAP_MS
#define CONTROLLER_GAP_MS 350
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRIES 8
#define WIFI_CONNECT_TIMEOUT_MS 45000
#define SWITCH_DEBOUNCE_MS 35

static const char *TAG = "on_air_sign";

typedef enum {
  SIGN_MODE_ON = 0,
  SIGN_MODE_GLOW = 1,
  SIGN_MODE_OFF = 2,
  SIGN_MODE_COUNT = 3,
} sign_mode_t;

typedef struct {
  sign_mode_t sign_mode;
  bool auto_enabled;
  bool meeting_active;
  char meeting_subject[96];
  char source[32];
} app_state_t;

static app_state_t g_state = {
    .sign_mode = SIGN_MODE_OFF,
    .auto_enabled = true,
    .meeting_active = false,
    .meeting_subject = "",
    .source = "boot",
};

static SemaphoreHandle_t g_state_mutex;
static SemaphoreHandle_t g_controller_mutex;
static EventGroupHandle_t g_wifi_event_group;
static int g_wifi_retry_count = 0;
static bool g_station_active = false;
static esp_netif_t *g_sta_netif = NULL;
static esp_netif_t *g_ap_netif = NULL;

static const char DASHBOARD_HTML[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>On Air Sign</title>"
    "<style>"
    ":root{color-scheme:dark;--bg:#111214;--panel:#1c1f24;--text:#f6f3ed;--muted:#9ca3af;--line:#343942;--red:#ff3b30;--amber:#ffb020;--blue:#4da3ff;}"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:var(--bg);color:var(--text);font:16px/1.4 system-ui,-apple-system,Segoe UI,sans-serif;display:grid;place-items:center;padding:20px;}"
    "main{width:min(560px,100%);display:grid;gap:14px}.status{border:1px solid var(--line);background:var(--panel);border-radius:8px;padding:22px;display:grid;gap:10px;}"
    ".lamp{height:150px;border-radius:8px;display:grid;place-items:center;background:#25282f;border:1px solid var(--line);font-size:clamp(42px,12vw,76px);font-weight:800;letter-spacing:0;}"
    ".lamp.on{background:var(--red);color:white;box-shadow:0 0 48px rgba(255,59,48,.35)}"
    ".lamp.glow{background:var(--amber);color:#1b1203;box-shadow:0 0 48px rgba(255,176,32,.32)}"
    ".meta{display:grid;grid-template-columns:1fr 1fr;gap:8px}.meta div{background:#15171b;border:1px solid var(--line);border-radius:8px;padding:10px;color:var(--muted);min-width:0}.meta strong{display:block;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
    ".controls{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}button{appearance:none;border:1px solid var(--line);border-radius:8px;background:#252a31;color:var(--text);padding:13px 12px;font:600 15px system-ui;cursor:pointer}button:hover{border-color:#606977}.primary{background:var(--blue);border-color:var(--blue);color:#06121f}.danger{background:var(--red);border-color:var(--red);color:white}"
    ".warning{background:var(--amber);border-color:var(--amber);color:#1b1203}"
    ".toggle{display:flex;align-items:center;justify-content:space-between;gap:14px;border:1px solid var(--line);background:var(--panel);border-radius:8px;padding:14px 16px}.toggle span{color:var(--muted)}input{width:44px;height:24px;accent-color:var(--blue)}"
    "@media(max-width:460px){.controls{grid-template-columns:1fr}.meta{grid-template-columns:1fr}.lamp{height:120px}}"
    "</style></head><body><main>"
    "<section class=\"status\"><div id=\"lamp\" class=\"lamp\">OFF</div>"
    "<div class=\"meta\"><div>Mode<strong id=\"mode\">-</strong></div><div>Meeting<strong id=\"meeting\">-</strong></div><div>Subject<strong id=\"subject\">-</strong></div><div>Source<strong id=\"source\">-</strong></div></div></section>"
    "<section class=\"controls\"><button class=\"danger\" data-action=\"on\">On</button><button class=\"warning\" data-action=\"glow\">Glow</button><button data-action=\"off\">Off</button></section>"
    "<label class=\"toggle\"><span>Follow Teams schedule</span><input id=\"auto\" type=\"checkbox\"></label>"
    "</main><script>"
    "const $=id=>document.getElementById(id);"
    "const names={on:'On',glow:'Glowing Pulse',off:'Off'},lampText={on:'ON AIR',glow:'PULSE',off:'OFF'};"
    "async function call(path){await fetch(path,{method:'POST'});await refresh();}"
    "async function refresh(){const r=await fetch('/api/status',{cache:'no-store'});const s=await r.json();"
    "const m=s.sign_mode||(s.sign_on?'on':'off');$('lamp').textContent=lampText[m]||m.toUpperCase();"
    "$('lamp').classList.toggle('on',m==='on');$('lamp').classList.toggle('glow',m==='glow');"
    "$('mode').textContent=(s.auto_enabled?'Auto':'Manual')+' / '+(names[m]||m);$('meeting').textContent=s.meeting_active?'Active':'Idle';"
    "$('subject').textContent=s.meeting_subject||'-';$('source').textContent=s.source||'-';$('auto').checked=s.auto_enabled;}"
    "document.querySelectorAll('button[data-action]').forEach(b=>b.onclick=()=>call('/api/'+b.dataset.action));"
    "$('auto').onchange=e=>call('/api/auto?enabled='+(e.target.checked?'1':'0'));"
    "refresh();setInterval(refresh,3000);"
    "</script></body></html>";

static void copy_text(char *dest, size_t dest_len, const char *src) {
  if (dest_len == 0) {
    return;
  }

  if (src == NULL) {
    dest[0] = '\0';
    return;
  }

  snprintf(dest, dest_len, "%s", src);
}

static void json_escape(const char *input, char *output, size_t output_len) {
  size_t write_pos = 0;

  if (output_len == 0) {
    return;
  }

  for (size_t i = 0; input != NULL && input[i] != '\0' && write_pos + 1 < output_len; ++i) {
    char c = input[i];
    if ((c == '"' || c == '\\') && write_pos + 2 < output_len) {
      output[write_pos++] = '\\';
      output[write_pos++] = c;
    } else if ((unsigned char)c >= 0x20) {
      output[write_pos++] = c;
    }
  }

  output[write_pos] = '\0';
}

static void apply_controller_press_output(bool pressed) {
  int level = pressed ? 1 : 0;
#if !CONTROLLER_PRESS_ACTIVE_HIGH
  level = !level;
#endif
  gpio_set_level((gpio_num_t)CONTROLLER_PRESS_GPIO, level);
}

static const char *sign_mode_id(sign_mode_t mode) {
  switch (mode) {
  case SIGN_MODE_ON:
    return "on";
  case SIGN_MODE_GLOW:
    return "glow";
  case SIGN_MODE_OFF:
  default:
    return "off";
  }
}

static sign_mode_t next_sign_mode(sign_mode_t mode) {
  return (sign_mode_t)(((int)mode + 1) % SIGN_MODE_COUNT);
}

static uint8_t controller_presses_to_mode(sign_mode_t current, sign_mode_t target) {
  return (uint8_t)(((int)target - (int)current + SIGN_MODE_COUNT) % SIGN_MODE_COUNT);
}

static void pulse_controller_switch(uint8_t press_count, const char *source) {
  if (press_count == 0) {
    return;
  }

  xSemaphoreTake(g_controller_mutex, portMAX_DELAY);

  for (uint8_t i = 0; i < press_count; ++i) {
    ESP_LOGI(TAG, "Pulsing controller switch %u/%u from %s", (unsigned)(i + 1), (unsigned)press_count,
             source == NULL ? "unknown" : source);
    apply_controller_press_output(true);
    vTaskDelay(pdMS_TO_TICKS(CONTROLLER_PRESS_MS));
    apply_controller_press_output(false);
    vTaskDelay(pdMS_TO_TICKS(i + 1 < press_count ? CONTROLLER_GAP_MS : 75));
  }

  xSemaphoreGive(g_controller_mutex);
}

static uint8_t set_assumed_mode_locked(sign_mode_t mode, const char *source) {
  uint8_t press_count = controller_presses_to_mode(g_state.sign_mode, mode);
  g_state.sign_mode = mode;
  copy_text(g_state.source, sizeof(g_state.source), source);
  return press_count;
}

static void set_manual_mode(sign_mode_t mode, const char *source) {
  uint8_t press_count = 0;

  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  g_state.auto_enabled = false;
  press_count = set_assumed_mode_locked(mode, source);
  xSemaphoreGive(g_state_mutex);

  pulse_controller_switch(press_count, source);
}

static void sync_assumed_mode(sign_mode_t mode, const char *source) {
  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  g_state.auto_enabled = false;
  g_state.sign_mode = mode;
  copy_text(g_state.source, sizeof(g_state.source), source);
  xSemaphoreGive(g_state_mutex);

  ESP_LOGI(TAG, "Synced assumed LED mode to %s without pressing controller switch", sign_mode_id(mode));
}

static void set_auto_enabled(bool enabled) {
  uint8_t press_count = 0;

  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  g_state.auto_enabled = enabled;
  copy_text(g_state.source, sizeof(g_state.source), enabled ? "auto" : "manual");
  if (enabled) {
    press_count = set_assumed_mode_locked(g_state.meeting_active ? SIGN_MODE_ON : SIGN_MODE_OFF, "teams");
  }
  xSemaphoreGive(g_state_mutex);

  pulse_controller_switch(press_count, "teams");
}

static void cycle_manual(const char *source) {
  sign_mode_t next_mode = SIGN_MODE_OFF;

  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  g_state.auto_enabled = false;
  next_mode = next_sign_mode(g_state.sign_mode);
  set_assumed_mode_locked(next_mode, source);
  xSemaphoreGive(g_state_mutex);

  pulse_controller_switch(1, source);
}

static void update_meeting(bool active, const char *subject) {
  uint8_t press_count = 0;

  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  g_state.meeting_active = active;
  copy_text(g_state.meeting_subject, sizeof(g_state.meeting_subject), subject);
  if (g_state.auto_enabled) {
    press_count = set_assumed_mode_locked(active ? SIGN_MODE_ON : SIGN_MODE_OFF, "teams");
  } else {
    copy_text(g_state.source, sizeof(g_state.source), "teams");
  }
  xSemaphoreGive(g_state_mutex);

  pulse_controller_switch(press_count, "teams");
}

static void configure_gpio(void) {
  gpio_config_t controller_press_config = {
      .pin_bit_mask = 1ULL << CONTROLLER_PRESS_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&controller_press_config));
  apply_controller_press_output(false);

  gpio_config_t switch_config = {
      .pin_bit_mask = 1ULL << SWITCH_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_ERROR_CHECK(gpio_config(&switch_config));
}

static bool parse_bool_value(const char *value, bool *out) {
  if (value == NULL || out == NULL) {
    return false;
  }

  if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "on") == 0 ||
      strcmp(value, "yes") == 0) {
    *out = true;
    return true;
  }

  if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "off") == 0 ||
      strcmp(value, "no") == 0) {
    *out = false;
    return true;
  }

  return false;
}

static bool parse_sign_mode_value(const char *value, sign_mode_t *out) {
  if (value == NULL || out == NULL) {
    return false;
  }

  if (strcmp(value, "on") == 0 || strcmp(value, "solid") == 0 || strcmp(value, "1") == 0) {
    *out = SIGN_MODE_ON;
    return true;
  }

  if (strcmp(value, "glow") == 0 || strcmp(value, "pulse") == 0 || strcmp(value, "glowing") == 0) {
    *out = SIGN_MODE_GLOW;
    return true;
  }

  if (strcmp(value, "off") == 0 || strcmp(value, "0") == 0) {
    *out = SIGN_MODE_OFF;
    return true;
  }

  return false;
}

static void get_network_status(char *output, size_t output_len) {
  wifi_mode_t mode;

  if (output_len == 0) {
    return;
  }

  if (esp_wifi_get_mode(&mode) != ESP_OK) {
    snprintf(output, output_len, "wifi=starting");
    return;
  }

  if ((mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) && g_sta_netif != NULL) {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(g_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
      snprintf(output, output_len, "wifi=sta ip=" IPSTR, IP2STR(&ip_info.ip));
      return;
    }
    snprintf(output, output_len, "wifi=sta ip=pending");
    return;
  }

  if ((mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) && g_ap_netif != NULL) {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(g_ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
      snprintf(output, output_len, "wifi=ap ip=" IPSTR, IP2STR(&ip_info.ip));
      return;
    }
    snprintf(output, output_len, "wifi=ap ip=192.168.4.1");
    return;
  }

  snprintf(output, output_len, "wifi=off");
}

static bool query_bool(httpd_req_t *req, const char *key, bool *out) {
  char query[160];
  char value[24];

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return false;
  }

  if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
    return false;
  }

  return parse_bool_value(value, out);
}

static bool query_sign_mode(httpd_req_t *req, sign_mode_t *out) {
  char query[160];
  char value[24];

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return false;
  }

  if (httpd_query_key_value(query, "mode", value, sizeof(value)) != ESP_OK &&
      httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK) {
    return false;
  }

  return parse_sign_mode_value(value, out);
}

static bool body_key_bool(const char *body, const char *key, bool *out) {
  const char *found = strstr(body, key);
  if (found == NULL) {
    return false;
  }

  const char *colon = strchr(found, ':');
  if (colon == NULL) {
    return false;
  }

  const char *value = colon + 1;
  while (*value == ' ' || *value == '\t' || *value == '"') {
    value++;
  }

  if (strncmp(value, "true", 4) == 0 || *value == '1') {
    *out = true;
    return true;
  }

  if (strncmp(value, "false", 5) == 0 || *value == '0') {
    *out = false;
    return true;
  }

  return false;
}

static bool body_bool(const char *body, bool *out) {
  if (body == NULL || out == NULL) {
    return false;
  }

  return body_key_bool(body, "meetingActive", out) || body_key_bool(body, "meeting_active", out) ||
         body_key_bool(body, "active", out);
}

static void body_subject(const char *body, char *subject, size_t subject_len) {
  const char *marker = "\"subject\"";
  const char *field = body == NULL ? NULL : strstr(body, marker);

  if (subject_len == 0) {
    return;
  }

  subject[0] = '\0';

  if (field == NULL) {
    return;
  }

  const char *colon = strchr(field + strlen(marker), ':');
  if (colon == NULL) {
    return;
  }

  const char *start = strchr(colon, '"');
  if (start == NULL) {
    return;
  }
  start++;

  size_t out = 0;
  for (const char *p = start; *p != '\0' && *p != '"' && out + 1 < subject_len; ++p) {
    if (*p == '\\' && p[1] != '\0') {
      p++;
    }
    subject[out++] = *p;
  }
  subject[out] = '\0';
}

static esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_len) {
  if (buffer_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  buffer[0] = '\0';

  if (req->content_len == 0) {
    return ESP_OK;
  }

  size_t remaining = req->content_len;
  size_t offset = 0;
  while (remaining > 0 && offset + 1 < buffer_len) {
    int received = httpd_req_recv(req, buffer + offset, buffer_len - offset - 1);
    if (received <= 0) {
      return ESP_FAIL;
    }
    offset += received;
    remaining -= received;
  }

  buffer[offset] = '\0';
  return ESP_OK;
}

static esp_err_t send_status_json(httpd_req_t *req) {
  app_state_t snapshot;
  char escaped_subject[160];
  char escaped_source[64];
  char response[512];

  xSemaphoreTake(g_state_mutex, portMAX_DELAY);
  snapshot = g_state;
  xSemaphoreGive(g_state_mutex);

  json_escape(snapshot.meeting_subject, escaped_subject, sizeof(escaped_subject));
  json_escape(snapshot.source, escaped_source, sizeof(escaped_source));

  snprintf(response, sizeof(response),
           "{\"sign_mode\":\"%s\",\"sign_on\":%s,\"sign_active\":%s,\"auto_enabled\":%s,"
           "\"meeting_active\":%s,\"meeting_subject\":\"%s\",\"source\":\"%s\","
           "\"controller_press_gpio\":%d,\"physical_switch_gpio\":%d,\"press_ms\":%d,\"gap_ms\":%d}",
           sign_mode_id(snapshot.sign_mode), snapshot.sign_mode == SIGN_MODE_ON ? "true" : "false",
           snapshot.sign_mode != SIGN_MODE_OFF ? "true" : "false", snapshot.auto_enabled ? "true" : "false",
           snapshot.meeting_active ? "true" : "false", escaped_subject, escaped_source, CONTROLLER_PRESS_GPIO,
           SWITCH_GPIO, CONTROLLER_PRESS_MS, CONTROLLER_GAP_MS);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_sendstr(req, response);
}

static esp_err_t root_handler(httpd_req_t *req) {
  const size_t html_len = strlen(DASHBOARD_HTML);
  const size_t chunk_size = 384;

  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  for (size_t offset = 0; offset < html_len; offset += chunk_size) {
    size_t remaining = html_len - offset;
    size_t send_len = remaining < chunk_size ? remaining : chunk_size;
    esp_err_t ret = httpd_resp_send_chunk(req, DASHBOARD_HTML + offset, send_len);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Dashboard response failed at byte %u/%u", (unsigned)offset, (unsigned)html_len);
      return ret;
    }
  }

  return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req) {
  return send_status_json(req);
}

static esp_err_t command_handler(httpd_req_t *req) {
  if (strcmp(req->uri, "/api/on") == 0) {
    set_manual_mode(SIGN_MODE_ON, "dashboard");
  } else if (strcmp(req->uri, "/api/glow") == 0 || strcmp(req->uri, "/api/pulse") == 0) {
    set_manual_mode(SIGN_MODE_GLOW, "dashboard");
  } else if (strcmp(req->uri, "/api/off") == 0) {
    set_manual_mode(SIGN_MODE_OFF, "dashboard");
  } else if (strcmp(req->uri, "/api/toggle") == 0 || strcmp(req->uri, "/api/next") == 0) {
    cycle_manual("dashboard");
  } else if (strncmp(req->uri, "/api/sync", 9) == 0) {
    sign_mode_t mode = SIGN_MODE_OFF;
    if (!query_sign_mode(req, &mode)) {
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_set_type(req, "application/json");
      return httpd_resp_sendstr(req, "{\"error\":\"missing mode\"}");
    }
    sync_assumed_mode(mode, "sync");
  } else if (strncmp(req->uri, "/api/auto", 9) == 0) {
    bool enabled = true;
    query_bool(req, "enabled", &enabled);
    set_auto_enabled(enabled);
  }

  return send_status_json(req);
}

static esp_err_t teams_handler(httpd_req_t *req) {
  bool active = false;
  bool found_active = query_bool(req, "active", &active);
  char subject[96] = "";
  char query[192];

  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "subject", subject, sizeof(subject));
  }

  if (!found_active) {
    char body[384];
    if (read_request_body(req, body, sizeof(body)) == ESP_OK) {
      found_active = body_bool(body, &active);
      if (subject[0] == '\0') {
        body_subject(body, subject, sizeof(subject));
      }
    }
  }

  if (!found_active) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"error\":\"missing active value\"}");
  }

  update_meeting(active, subject);
  return send_status_json(req);
}

static void start_http_server(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 12;

  ESP_ERROR_CHECK(httpd_start(&server, &config));

  const httpd_uri_t root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = root_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));

  const httpd_uri_t status = {
      .uri = "/api/status",
      .method = HTTP_GET,
      .handler = status_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));

  const httpd_uri_t on = {
      .uri = "/api/on",
      .method = HTTP_POST,
      .handler = command_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &on));

  const httpd_uri_t glow = {
      .uri = "/api/glow",
      .method = HTTP_POST,
      .handler = command_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &glow));

  const httpd_uri_t pulse = {
      .uri = "/api/pulse",
      .method = HTTP_POST,
      .handler = command_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &pulse));

  const httpd_uri_t off = {
      .uri = "/api/off",
      .method = HTTP_POST,
      .handler = command_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &off));

  const httpd_uri_t toggle = {
      .uri = "/api/toggle",
      .method = HTTP_POST,
      .handler = command_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &toggle));

  const httpd_uri_t next = {
      .uri = "/api/next",
      .method = HTTP_POST,
      .handler = command_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &next));

  const httpd_uri_t sync = {
      .uri = "/api/sync*",
      .method = HTTP_POST,
      .handler = command_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sync));

  const httpd_uri_t auto_mode = {
      .uri = "/api/auto*",
      .method = HTTP_POST,
      .handler = command_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &auto_mode));

  const httpd_uri_t teams = {
      .uri = "/api/teams*",
      .method = HTTP_POST,
      .handler = teams_handler,
      .user_ctx = NULL,
  };
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &teams));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (!g_station_active) {
      return;
    } else if (g_wifi_retry_count < WIFI_MAX_RETRIES) {
      esp_wifi_connect();
      g_wifi_retry_count++;
      ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d/%d)", g_wifi_retry_count, WIFI_MAX_RETRIES);
    } else {
      xEventGroupSetBits(g_wifi_event_group, WIFI_FAIL_BIT);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Wi-Fi connected at " IPSTR, IP2STR(&event->ip_info.ip));
    g_wifi_retry_count = 0;
    xEventGroupSetBits(g_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static bool start_station(void) {
  if (strlen(WIFI_SSID) == 0) {
    ESP_LOGW(TAG, "No WIFI_SSID configured; starting fallback access point");
    return false;
  }

  xEventGroupClearBits(g_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  g_wifi_retry_count = 0;
  g_station_active = true;

  wifi_config_t wifi_config = {0};
  snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", WIFI_SSID);
  snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", WIFI_PASSWORD);
  wifi_config.sta.threshold.authmode = strlen(WIFI_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

  ESP_LOGI(TAG, "Connecting to configured Wi-Fi network");
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  EventBits_t bits = xEventGroupWaitBits(g_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                         pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

  if ((bits & WIFI_CONNECTED_BIT) != 0) {
    return true;
  }

  ESP_LOGW(TAG, "Wi-Fi station connection failed or timed out; starting fallback access point");
  g_station_active = false;
  esp_wifi_stop();
  return false;
}

static void start_softap(void) {
  uint8_t mac[6] = {0};
  char ssid[32];

  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
  snprintf(ssid, sizeof(ssid), "OnAirSign-%02X%02X", mac[4], mac[5]);

  wifi_config_t ap_config = {0};
  snprintf((char *)ap_config.ap.ssid, sizeof(ap_config.ap.ssid), "%s", ssid);
  ap_config.ap.ssid_len = strlen(ssid);
  snprintf((char *)ap_config.ap.password, sizeof(ap_config.ap.password), "%s", ON_AIR_AP_PASSWORD);
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = strlen(ON_AIR_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGW(TAG, "Fallback AP started: SSID %s, dashboard http://192.168.4.1", ssid);
}

static void init_wifi(void) {
  g_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  g_sta_netif = esp_netif_create_default_wifi_sta();
  g_ap_netif = esp_netif_create_default_wifi_ap();
  ESP_ERROR_CHECK(esp_netif_set_hostname(g_sta_netif, DEVICE_HOSTNAME));
  ESP_ERROR_CHECK(esp_netif_set_hostname(g_ap_netif, DEVICE_HOSTNAME));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

  if (!start_station()) {
    start_softap();
  }
}

static void init_mdns(void) {
  ESP_ERROR_CHECK(mdns_init());
  ESP_ERROR_CHECK(mdns_hostname_set(DEVICE_HOSTNAME));
  ESP_ERROR_CHECK(mdns_instance_name_set("On Air Sign"));
  ESP_ERROR_CHECK(mdns_service_add("On Air Sign", "_http", "_tcp", 80, NULL, 0));

  ESP_LOGI(TAG, "mDNS started: http://%s.local/", DEVICE_HOSTNAME);
}

static void switch_task(void *arg) {
  bool last_level = gpio_get_level((gpio_num_t)SWITCH_GPIO);
  bool stable_level = last_level;
  int64_t last_change_us = esp_timer_get_time();

  while (true) {
    bool current_level = gpio_get_level((gpio_num_t)SWITCH_GPIO);
    int64_t now_us = esp_timer_get_time();

    if (current_level != last_level) {
      last_level = current_level;
      last_change_us = now_us;
    }

    if (current_level != stable_level && (now_us - last_change_us) > SWITCH_DEBOUNCE_MS * 1000) {
      stable_level = current_level;
      if (!stable_level) {
        cycle_manual("switch");
        ESP_LOGI(TAG, "Switch pressed");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void heartbeat_task(void *arg) {
  while (true) {
    app_state_t snapshot;
    char network_status[48];

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    snapshot = g_state;
    xSemaphoreGive(g_state_mutex);
    get_network_status(network_status, sizeof(network_status));

    ESP_LOGI(TAG, "Heartbeat: sign=%s auto=%s meeting=%s source=%s %s",
             sign_mode_id(snapshot.sign_mode),
             snapshot.auto_enabled ? "on" : "off",
             snapshot.meeting_active ? "active" : "idle",
             snapshot.source,
             network_status);

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

static void init_nvs(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
}

void app_main(void) {
  init_nvs();

  g_state_mutex = xSemaphoreCreateMutex();
  g_controller_mutex = xSemaphoreCreateMutex();
  if (g_state_mutex == NULL || g_controller_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create mutexes");
    abort();
  }

  configure_gpio();
  init_wifi();
  init_mdns();
  start_http_server();

  xTaskCreate(switch_task, "switch_task", 3072, NULL, 5, NULL);
  xTaskCreate(heartbeat_task, "heartbeat_task", 3072, NULL, 4, NULL);

  ESP_LOGI(TAG, "On Air Sign ready. Controller press GPIO=%d, physical switch GPIO=%d",
           CONTROLLER_PRESS_GPIO, SWITCH_GPIO);
}
