// ============================================================================
// claude-status firmware — entry point and render loop.
//
// Architecture overview:
//
//   serial_reader_task  ──reads NDJSON──▶  handle_line()
//                                              │
//                                              ├─ apply_state_update()    (mutates g_clients[slot])
//                                              ├─ handle_ping()           (writes pong reply)
//                                              ├─ handle_resync()         (writes per-slot state)
//                                              ├─ handle_configure()      (validates → swaps driver → NVS)
//                                              └─ handle_identify()       (sets g_identify_until_us)
//
//   render loop (in app_main)
//        │
//        ├─ snapshot g_clients under g_mutex
//        ├─ for each panel: render slot(s) into its viewport
//        └─ flip_buffer
//
// Wire contract (host→device JSON, device→host responses) is canonical
// in the bridge repo:
//   https://github.com/sep/cc-status-bridge/blob/main/docs/FIRMWARE.md
// Board-specific GPIO assignments live in board_config.h. Coding
// conventions: see /CONTRIBUTING.md.
// ============================================================================

#include "hub75.h"

#include "board_config.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "font5x7.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <cmath>
#include <cstdio>
#include <cstring>

static const char *TAG = "claude-status";

// ======================================================================
// Panel layout — runtime-mutable via configure command (§8), persisted in
// NVS so reboots restore the last-applied config. MAX_PANELS bounds the
// static client slot array and can't change at runtime.
// ======================================================================

static constexpr int MAX_PANELS = 4;

// Compile-time defaults used on very first boot (no NVS record yet).
static constexpr int      DEFAULT_PANEL_COUNT  = 1;
static constexpr int      DEFAULT_FIRST_ID     = 1;
static constexpr uint16_t DEFAULT_PANEL_WIDTH  = 64;
static constexpr uint16_t DEFAULT_PANEL_HEIGHT = 32;

// Current live layout.
static int      g_panel_count  = DEFAULT_PANEL_COUNT;
static int      g_first_id     = DEFAULT_FIRST_ID;
static uint16_t g_panel_width  = DEFAULT_PANEL_WIDTH;
static uint16_t g_panel_height = DEFAULT_PANEL_HEIGHT;

// Each physical panel owns three possible slots: FULL, HALF_A (left 32 px),
// HALF_B (right 32 px). FULL and the two halves are mutually exclusive on a
// given panel — the most recently updated kind wins, the other is wiped.
enum SlotKind : uint8_t { SLOT_FULL = 0, SLOT_HALF_A = 1, SLOT_HALF_B = 2 };
static constexpr uint8_t SLOT_KIND_COUNT = 3;
static constexpr int     TOTAL_SLOTS    = MAX_PANELS * SLOT_KIND_COUNT;

static inline int slot_index(uint8_t panel_idx, SlotKind kind) {
  return panel_idx * SLOT_KIND_COUNT + static_cast<int>(kind);
}

// ======================================================================
// State lexicon — wire spec §4
// ======================================================================

enum class Status : uint8_t {
  UNKNOWN = 0,
  IDLE,
  WORKING,
  THINKING,
  BLOCKED,
  COMPACTING,
  ERROR,
};

static const char *status_name(Status s) {
  switch (s) {
    case Status::UNKNOWN:    return "unknown";
    case Status::IDLE:       return "idle";
    case Status::WORKING:    return "working";
    case Status::THINKING:   return "thinking";
    case Status::BLOCKED:    return "blocked";
    case Status::COMPACTING: return "compacting";
    case Status::ERROR:      return "error";
  }
  return "?";
}

static bool parse_status_string(const char *s, Status *out) {
  if (!s) return false;
  if (strcmp(s, "idle")       == 0) { *out = Status::IDLE;       return true; }
  if (strcmp(s, "working")    == 0) { *out = Status::WORKING;    return true; }
  if (strcmp(s, "thinking")   == 0) { *out = Status::THINKING;   return true; }
  if (strcmp(s, "blocked")    == 0) { *out = Status::BLOCKED;    return true; }
  if (strcmp(s, "compacting") == 0) { *out = Status::COMPACTING; return true; }
  if (strcmp(s, "error")      == 0) { *out = Status::ERROR;      return true; }
  return false;
}

// ======================================================================
// Shared state — one ClientSnapshot per slot (FULL/HALF_A/HALF_B per panel).
// ======================================================================

struct ClientSnapshot {
  bool     pinned               = false;
  Status   state                = Status::UNKNOWN;
  int64_t  state_changed_at_us  = 0;
  int64_t  last_line_at_us      = 0;
  int      subagent_count       = 0;
  int      tasks_active         = 0;  // pending + in_progress in the session's plan
  int      tasks_completed      = 0;  // tasks marked done this session
  char     last_event[32]       = {0};
};

static ClientSnapshot    g_clients[TOTAL_SLOTS];
static SemaphoreHandle_t g_mutex          = nullptr; // guards g_clients, runtime config vars, g_identify_until_us, g_blocked_ack_active
static SemaphoreHandle_t g_driver_mutex   = nullptr; // guards g_driver during swap
static Hub75Driver      *g_driver         = nullptr;
static int64_t           g_identify_until_us = 0;    // 0 = no identify pending; else esp_timer_get_time deadline
static bool              g_blocked_ack_active = false;
static volatile int64_t  g_last_rx_us         = 0;    // most recent serial-byte arrival; drives render_conn_dot
static volatile int64_t  g_last_activity_us   = 0;    // most recent state-change or ack press; drives screensaver
static constexpr int64_t SCREENSAVER_TIMEOUT_US = 5LL * 60LL * 1'000'000LL;  // 5 minutes
static constexpr int     SCREENSAVER_DIM_NUM     = 1;
static constexpr int     SCREENSAVER_DIM_DEN     = 10;                       // 10% of bright
// ESP-wide ack flag. Set by the ack button when at least one slot is BLOCKED.
// Causes BLOCKED renders to render in a calm, non-strobing variant. Cleared
// automatically when no slot is currently BLOCKED — so a new blocked event
// after a full clear re-alarms fresh.

// ======================================================================
// Viewport — a rectangular slice of the framebuffer that a slot renders into.
// Renderers take (x, y) local to the viewport; clip against (w, h); and the
// set_pixel call translates to (vp.x0 + x, vp.y0 + y).
// ======================================================================

struct Viewport { int16_t x0, y0, w, h; };

static Viewport viewport_for(uint8_t panel_idx, SlotKind kind) {
  const int16_t pw  = static_cast<int16_t>(g_panel_width);
  const int16_t ph  = static_cast<int16_t>(g_panel_height);
  const int16_t px0 = static_cast<int16_t>(panel_idx * pw);
  switch (kind) {
    case SLOT_FULL:   return {px0,                            0, pw,     ph};
    case SLOT_HALF_A: return {px0,                            0, (int16_t)(pw / 2), ph};
    case SLOT_HALF_B: return {static_cast<int16_t>(px0 + pw / 2),
                                                              0, (int16_t)(pw / 2), ph};
  }
  return {px0, 0, pw, ph};
}

// ======================================================================
// Client ID parsing — "1", "1a", "1b", "2a", etc. Returns panel_idx within
// this firmware's owned range. Lines targeting IDs we don't own are rejected.
// ======================================================================

static bool parse_client_id(const char *id, uint8_t *out_panel, SlotKind *out_kind) {
  if (!id || !*id) return false;

  int n = 0;
  const char *p = id;
  if (*p < '0' || *p > '9') return false;
  while (*p >= '0' && *p <= '9') {
    n = n * 10 + (*p - '0');
    p++;
    if (n > 99) return false;
  }
  if (n < g_first_id || n >= g_first_id + g_panel_count) return false;

  *out_panel = static_cast<uint8_t>(n - g_first_id);

  if (*p == '\0')                         { *out_kind = SLOT_FULL;   return true; }
  if (*p == 'a' && *(p + 1) == '\0')      { *out_kind = SLOT_HALF_A; return true; }
  if (*p == 'b' && *(p + 1) == '\0')      { *out_kind = SLOT_HALF_B; return true; }
  return false;
}

// Format a slot back into its wire ID string (fits in 8 bytes easily).
static void format_client_id(uint8_t panel_idx, SlotKind kind, char *out, size_t cap) {
  const int panel_num = g_first_id + panel_idx;
  const char suffix = (kind == SLOT_FULL) ? '\0' : (kind == SLOT_HALF_A ? 'a' : 'b');
  if (suffix) snprintf(out, cap, "%d%c", panel_num, suffix);
  else        snprintf(out, cap, "%d",   panel_num);
}

// ======================================================================
// Outgoing messages — write JSON replies back over USB-Serial-JTAG.
// We hand-roll the JSON to avoid cJSON allocation churn in the hot path.
// ======================================================================

static void write_json_line(const char *buf, int n) {
  if (n <= 0) return;
  usb_serial_jtag_write_bytes(buf, static_cast<size_t>(n), pdMS_TO_TICKS(20));
}

static void send_pong(int64_t seq) {
  // Snapshot the entire client table under the mutex first, then serialize
  // without holding the lock (JSON writes can take longer than the 10 ms
  // mutex budget, and the wire format doesn't need a consistent "now").
  ClientSnapshot local[TOTAL_SLOTS];
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    memcpy(local, g_clients, sizeof(local));
    xSemaphoreGive(g_mutex);
  } else {
    return;  // mutex contended; skip this pong, bridge will retry
  }

  const int64_t uptime_ms = esp_timer_get_time() / 1000;
  // Static so we don't burn 2 KB of serial_reader_task's stack every ping.
  // Safe because send_pong only ever runs on that one task — handle_ping
  // is called from handle_line which is called from serial_reader_task.
  static char buf[2048];
  int n = snprintf(buf, sizeof(buf),
      "{\"type\":\"pong\",\"seq\":%lld,\"panel_count\":%d,\"first_id\":%d,\"clients\":{",
      static_cast<long long>(seq), g_panel_count, g_first_id);

  bool first = true;
  for (uint8_t p = 0; p < g_panel_count; p++) {
    for (uint8_t k = 0; k < SLOT_KIND_COUNT; k++) {
      const ClientSnapshot &cs = local[slot_index(p, static_cast<SlotKind>(k))];
      if (!cs.pinned) continue;

      char id[8];
      format_client_id(p, static_cast<SlotKind>(k), id, sizeof(id));
      n += snprintf(buf + n, sizeof(buf) - n,
          "%s\"%s\":{\"state\":\"%s\",\"subagent_count\":%d,"
          "\"tasks_active\":%d,\"tasks_completed\":%d,"
          "\"uptime_ms_at_change\":%lld}",
          first ? "" : ",",
          id,
          status_name(cs.state),
          cs.subagent_count,
          cs.tasks_active,
          cs.tasks_completed,
          static_cast<long long>(cs.state_changed_at_us / 1000));
      first = false;
      if (n >= static_cast<int>(sizeof(buf)) - 64) break;  // guardrail
    }
  }
  n += snprintf(buf + n, sizeof(buf) - n,
      "},\"uptime_ms\":%lld}\n", static_cast<long long>(uptime_ms));
  write_json_line(buf, n);
}

static void send_spontaneous_state_for(uint8_t panel_idx, SlotKind kind,
                                       const ClientSnapshot &cs) {
  char id[8];
  format_client_id(panel_idx, kind, id, sizeof(id));
  char buf[256];
  const int n = snprintf(buf, sizeof(buf),
      "{\"type\":\"state\",\"client\":\"%s\",\"state\":\"%s\","
      "\"subagent_count\":%d,\"tasks_active\":%d,\"tasks_completed\":%d,"
      "\"uptime_ms\":%lld}\n",
      id, status_name(cs.state),
      cs.subagent_count, cs.tasks_active, cs.tasks_completed,
      static_cast<long long>(esp_timer_get_time() / 1000));
  write_json_line(buf, n);
}

// ======================================================================
// Hub75 driver config factory — centralizes the "config from runtime vars"
// conversion so initial boot and post-configure rebuild use the same shape.
// Pins are firmware-level; never configurable by the bridge.
// ======================================================================

static Hub75Config build_hub_cfg() {
  Hub75Config cfg{};
  cfg.panel_width   = g_panel_width;
  cfg.panel_height  = g_panel_height;
  cfg.layout_cols   = g_panel_count;
  cfg.layout_rows   = 1;
  cfg.layout        = Hub75PanelLayout::HORIZONTAL;
  cfg.scan_wiring   = Hub75ScanWiring::STANDARD_TWO_SCAN;
  cfg.shift_driver  = Hub75ShiftDriver::GENERIC;
  cfg.brightness    = 64;
  cfg.double_buffer = true;

  cfg.pins.r1 = BOARD_PINS.r1;  cfg.pins.g1 = BOARD_PINS.g1;  cfg.pins.b1 = BOARD_PINS.b1;
  cfg.pins.r2 = BOARD_PINS.r2;  cfg.pins.g2 = BOARD_PINS.g2;  cfg.pins.b2 = BOARD_PINS.b2;
  cfg.pins.a  = BOARD_PINS.a;   cfg.pins.b  = BOARD_PINS.b;
  cfg.pins.c  = BOARD_PINS.c;   cfg.pins.d  = BOARD_PINS.d;
  cfg.pins.e  = BOARD_PINS.e;
  cfg.pins.clk = BOARD_PINS.clk; cfg.pins.lat = BOARD_PINS.lat; cfg.pins.oe = BOARD_PINS.oe;
  return cfg;
}

// ======================================================================
// NVS-backed config persistence. Reads and writes panel_count / first_id /
// panel_width / panel_height; layout is always "horizontal" so we don't
// bother storing it.
// ======================================================================

static constexpr const char *NVS_NAMESPACE = "claude-status";

static bool nvs_load_config() {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

  uint8_t  pc8 = 0, fid8 = 0;
  uint16_t pw16 = 0, ph16 = 0;
  const esp_err_t e_pc  = nvs_get_u8 (h, "panel_count",  &pc8);
  const esp_err_t e_fid = nvs_get_u8 (h, "first_id",     &fid8);
  const esp_err_t e_pw  = nvs_get_u16(h, "panel_width",  &pw16);
  const esp_err_t e_ph  = nvs_get_u16(h, "panel_height", &ph16);
  nvs_close(h);

  // Partial records count as "not present" — fall back to defaults.
  if (e_pc != ESP_OK || e_fid != ESP_OK || e_pw != ESP_OK || e_ph != ESP_OK) {
    return false;
  }

  // Validate on load too: a corrupt or forward-version NVS record shouldn't
  // panic the firmware at boot. Same rules as configure-time validation.
  if (pc8 < 1 || pc8 > MAX_PANELS) return false;
  if (fid8 < 1 || fid8 > 99) return false;
  if (pw16 != 32 && pw16 != 64) return false;
  if (ph16 != 16 && ph16 != 32 && ph16 != 64) return false;

  g_panel_count  = pc8;
  g_first_id     = fid8;
  g_panel_width  = pw16;
  g_panel_height = ph16;
  return true;
}

static esp_err_t nvs_save_config() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_u8 (h, "panel_count",  static_cast<uint8_t>(g_panel_count));
  if (err == ESP_OK) err = nvs_set_u8 (h, "first_id",     static_cast<uint8_t>(g_first_id));
  if (err == ESP_OK) err = nvs_set_u16(h, "panel_width",  g_panel_width);
  if (err == ESP_OK) err = nvs_set_u16(h, "panel_height", g_panel_height);
  if (err == ESP_OK) err = nvs_commit(h);

  nvs_close(h);
  return err;
}

// ======================================================================
// Configure command — validate, apply, persist, ack.
// ======================================================================

struct PendingConfig {
  int      panel_count;
  int      first_id;
  uint16_t panel_width;
  uint16_t panel_height;
};

static bool validate_configure(const cJSON *root, PendingConfig *out) {
  const cJSON *pc = cJSON_GetObjectItemCaseSensitive(root, "panel_count");
  const cJSON *pw = cJSON_GetObjectItemCaseSensitive(root, "panel_width");
  const cJSON *ph = cJSON_GetObjectItemCaseSensitive(root, "panel_height");
  const cJSON *ly = cJSON_GetObjectItemCaseSensitive(root, "layout");
  const cJSON *fi = cJSON_GetObjectItemCaseSensitive(root, "first_id");

  if (!cJSON_IsNumber(pc) || !cJSON_IsNumber(pw) || !cJSON_IsNumber(ph) ||
      !cJSON_IsString(ly) || !cJSON_IsNumber(fi)) {
    return false;
  }

  const int panel_count  = static_cast<int>(cJSON_GetNumberValue(pc));
  const int first_id     = static_cast<int>(cJSON_GetNumberValue(fi));
  const int panel_width  = static_cast<int>(cJSON_GetNumberValue(pw));
  const int panel_height = static_cast<int>(cJSON_GetNumberValue(ph));
  const char *layout     = ly->valuestring;

  if (panel_count < 1 || panel_count > MAX_PANELS)     return false;
  if (first_id < 1 || first_id > 99)                   return false;
  if (panel_width != 32 && panel_width != 64)          return false;
  if (panel_height != 16 && panel_height != 32 && panel_height != 64) return false;
  if (strcmp(layout, "horizontal") != 0)               return false;

  out->panel_count  = panel_count;
  out->first_id     = first_id;
  out->panel_width  = static_cast<uint16_t>(panel_width);
  out->panel_height = static_cast<uint16_t>(panel_height);
  return true;
}

static void send_configured_ack(bool changed) {
  char buf[256];
  const int n = snprintf(buf, sizeof(buf),
      "{\"type\":\"configured\",\"panel_count\":%d,\"panel_width\":%u,\"panel_height\":%u,"
      "\"layout\":\"horizontal\",\"first_id\":%d,\"changed\":%s,\"uptime_ms\":%lld}\n",
      g_panel_count, g_panel_width, g_panel_height, g_first_id,
      changed ? "true" : "false",
      static_cast<long long>(esp_timer_get_time() / 1000));
  write_json_line(buf, n);
}

static void handle_configure(const cJSON *root) {
  PendingConfig pending;
  if (!validate_configure(root, &pending)) return;  // silently dropped

  // No-op short-circuit.
  if (pending.panel_count  == g_panel_count &&
      pending.first_id     == g_first_id &&
      pending.panel_width  == g_panel_width &&
      pending.panel_height == g_panel_height) {
    send_configured_ack(false);
    return;
  }

  // Serialize driver swap through g_driver_mutex. Render task blocks for
  // the duration of teardown + rebuild (~tens of ms). This runs on the
  // serial reader task, which the watchdog tracks — swap time << WDT
  // timeout, so we're safe not to poke the WDT mid-swap.
  xSemaphoreTake(g_driver_mutex, portMAX_DELAY);

  // Update runtime vars + wipe newly out-of-range slots under g_mutex.
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    g_panel_count  = pending.panel_count;
    g_first_id     = pending.first_id;
    g_panel_width  = pending.panel_width;
    g_panel_height = pending.panel_height;

    for (int p = g_panel_count; p < MAX_PANELS; p++) {
      for (int k = 0; k < SLOT_KIND_COUNT; k++) {
        g_clients[slot_index(p, static_cast<SlotKind>(k))] = ClientSnapshot{};
      }
    }
    xSemaphoreGive(g_mutex);
  }

  if (nvs_save_config() != ESP_OK) {
    ESP_LOGW(TAG, "nvs_save_config() failed; config applied but not persisted");
  }

  if (g_driver) {
    g_driver->end();
    delete g_driver;
    g_driver = nullptr;
  }

  g_driver = new Hub75Driver(build_hub_cfg());
  if (!g_driver->begin()) {
    ESP_LOGE(TAG, "Hub75Driver::begin() failed after configure — restarting");
    xSemaphoreGive(g_driver_mutex);
    esp_restart();
  }

  xSemaphoreGive(g_driver_mutex);

  ESP_LOGI(TAG, "configure applied: panels=%d first_id=%d %ux%u",
           g_panel_count, g_first_id, g_panel_width, g_panel_height);
  send_configured_ack(true);
}

// ======================================================================
// Line handler — parse JSON once, then dispatch by `type` per wire spec §8.
// ======================================================================

static void apply_state_update(const cJSON *root) {
  const cJSON *state_node = cJSON_GetObjectItemCaseSensitive(root, "state");
  Status new_state;
  if (!cJSON_IsString(state_node) ||
      !parse_status_string(state_node->valuestring, &new_state)) {
    return;
  }

  // Resolve target slot. Missing `client` → legacy fallback = lowest-ID FULL.
  const cJSON *client_node = cJSON_GetObjectItemCaseSensitive(root, "client");
  uint8_t panel = 0;
  SlotKind kind = SLOT_FULL;
  if (cJSON_IsString(client_node)) {
    if (!parse_client_id(client_node->valuestring, &panel, &kind)) {
      return;  // unknown / unowned / malformed slot → drop silently (§3.1)
    }
  }

  const cJSON *event_node = cJSON_GetObjectItemCaseSensitive(root, "event");
  const char *event_str = cJSON_IsString(event_node) ? event_node->valuestring : nullptr;

  const cJSON *count_node = cJSON_GetObjectItemCaseSensitive(root, "subagent_count");
  int new_count = 0;
  bool have_count = cJSON_IsNumber(count_node);
  if (have_count) {
    const int v = static_cast<int>(cJSON_GetNumberValue(count_node));
    new_count = v < 0 ? 0 : v;
  }

  const cJSON *active_node = cJSON_GetObjectItemCaseSensitive(root, "tasks_active");
  int new_active = 0;
  bool have_active = cJSON_IsNumber(active_node);
  if (have_active) {
    const int v = static_cast<int>(cJSON_GetNumberValue(active_node));
    new_active = v < 0 ? 0 : v;
  }

  const cJSON *completed_node = cJSON_GetObjectItemCaseSensitive(root, "tasks_completed");
  int new_completed = 0;
  bool have_completed = cJSON_IsNumber(completed_node);
  if (have_completed) {
    const int v = static_cast<int>(cJSON_GetNumberValue(completed_node));
    new_completed = v < 0 ? 0 : v;
  }

  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    const int64_t now_us = esp_timer_get_time();

    // Mutual exclusion: FULL vs halves on the same panel.
    if (kind == SLOT_FULL) {
      g_clients[slot_index(panel, SLOT_HALF_A)].pinned = false;
      g_clients[slot_index(panel, SLOT_HALF_B)].pinned = false;
    } else {
      g_clients[slot_index(panel, SLOT_FULL)].pinned = false;
    }

    ClientSnapshot &cs = g_clients[slot_index(panel, kind)];
    const bool state_changed = (!cs.pinned) || (cs.state != new_state);
    if (state_changed) {
      char id[8];
      format_client_id(panel, kind, id, sizeof(id));
      ESP_LOGI(TAG, "[%s] state: %s -> %s%s%s",
               id,
               cs.pinned ? status_name(cs.state) : "unknown",
               status_name(new_state),
               event_str ? " event=" : "", event_str ? event_str : "");
      cs.state = new_state;
      cs.state_changed_at_us = now_us;
      g_last_activity_us = now_us;
    }
    cs.pinned = true;
    cs.last_line_at_us = now_us;
    if (have_count)     cs.subagent_count  = new_count;
    if (have_active)    cs.tasks_active    = new_active;
    if (have_completed) cs.tasks_completed = new_completed;
    if (event_str) {
      strncpy(cs.last_event, event_str, sizeof(cs.last_event) - 1);
      cs.last_event[sizeof(cs.last_event) - 1] = '\0';
    }
    xSemaphoreGive(g_mutex);
  }
}

static void handle_ping(const cJSON *root) {
  const cJSON *seq_node = cJSON_GetObjectItemCaseSensitive(root, "seq");
  int64_t seq = 0;
  if (cJSON_IsNumber(seq_node)) {
    seq = static_cast<int64_t>(cJSON_GetNumberValue(seq_node));
  }
  send_pong(seq);
}

static void handle_identify(const cJSON *root) {
  // Optional duration_ms; default 5000; clamped to [500, 30000] so a typo
  // can't wedge the display.
  int64_t duration_ms = 5000;
  const cJSON *dur_node = cJSON_GetObjectItemCaseSensitive(root, "duration_ms");
  if (cJSON_IsNumber(dur_node)) {
    duration_ms = static_cast<int64_t>(cJSON_GetNumberValue(dur_node));
  }
  if (duration_ms < 500)   duration_ms = 500;
  if (duration_ms > 30000) duration_ms = 30000;

  const int64_t deadline = esp_timer_get_time() + duration_ms * 1000;
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    g_identify_until_us = deadline;
    xSemaphoreGive(g_mutex);
  }
  ESP_LOGI(TAG, "identify: %lld ms", static_cast<long long>(duration_ms));
}

static void handle_resync() {
  ClientSnapshot local[TOTAL_SLOTS];
  if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    memcpy(local, g_clients, sizeof(local));
    xSemaphoreGive(g_mutex);
  } else {
    return;
  }
  for (uint8_t p = 0; p < g_panel_count; p++) {
    for (uint8_t k = 0; k < SLOT_KIND_COUNT; k++) {
      const ClientSnapshot &cs = local[slot_index(p, static_cast<SlotKind>(k))];
      if (cs.pinned) send_spontaneous_state_for(p, static_cast<SlotKind>(k), cs);
    }
  }
}

static void handle_line(const char *line, size_t len) {
  if (len == 0) return;

  cJSON *root = cJSON_Parse(line);
  if (!root) return;  // malformed JSON

  const cJSON *type_node = cJSON_GetObjectItemCaseSensitive(root, "type");
  const char *type_str = cJSON_IsString(type_node) ? type_node->valuestring : nullptr;

  if (!type_str) {
    if (cJSON_HasObjectItem(root, "state")) apply_state_update(root);
  } else if (strcmp(type_str, "state") == 0) {
    apply_state_update(root);
  } else if (strcmp(type_str, "ping") == 0) {
    handle_ping(root);
  } else if (strcmp(type_str, "resync") == 0) {
    handle_resync();
  } else if (strcmp(type_str, "configure") == 0) {
    handle_configure(root);
  } else if (strcmp(type_str, "identify") == 0) {
    handle_identify(root);
  }
  // Unknown types: ignore for forward compat.

  cJSON_Delete(root);
}

// ======================================================================
// Serial reader task.
// ======================================================================

static constexpr size_t LINE_BUF_MAX = 512;

static void serial_reader_task(void *) {
  char   line[LINE_BUF_MAX];
  size_t len = 0;
  uint8_t chunk[128];

  ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));

  ESP_LOGI(TAG, "serial_reader_task: up");
  while (true) {
    esp_task_wdt_reset();
    const int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(1000));
    if (n <= 0) continue;
    g_last_rx_us = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
      const char c = static_cast<char>(chunk[i]);
      if (c == '\r') continue;
      if (c == '\n') {
        line[len] = '\0';
        handle_line(line, len);
        len = 0;
      } else if (len < LINE_BUF_MAX - 1) {
        line[len++] = c;
      } else {
        len = 0;  // too-long line; resync on next '\n'
      }
    }
  }
}

// ======================================================================
// Ack button — momentary push button that silences the BLOCKED-state
// strobe across every slot driven by this ESP. Polled with software
// debounce. Active-low; uses internal pull-up. Default GPIO is the BOOT
// button on most ESP32-S3 dev boards (configured per board in
// board_config.h). Set BoardPins.ack_button to -1 to disable.
// ======================================================================

static void ack_button_task(void *) {
  if (BOARD_PINS.ack_button < 0) {
    ESP_LOGI(TAG, "ack_button_task: disabled (ack_button = -1)");
    vTaskDelete(nullptr);
    return;
  }

  const gpio_num_t pin = static_cast<gpio_num_t>(BOARD_PINS.ack_button);
  gpio_config_t btn_cfg = {};
  btn_cfg.pin_bit_mask = 1ULL << BOARD_PINS.ack_button;
  btn_cfg.mode = GPIO_MODE_INPUT;
  btn_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  btn_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  btn_cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&btn_cfg);

  // Pulled-up = HIGH = not pressed; LOW = pressed.
  bool last_steady    = true;   // last debounced state
  bool candidate      = true;   // most recent raw read
  int  candidate_runs = 0;      // consecutive matches at the new level

  ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
  ESP_LOGI(TAG, "ack_button_task: up (GPIO %d)", BOARD_PINS.ack_button);

  while (true) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(20));

    const bool current = gpio_get_level(pin) != 0;
    if (current == last_steady) {
      // Bouncing back to the steady state — reset the candidate counter.
      candidate = current;
      candidate_runs = 0;
      continue;
    }
    if (current != candidate) {
      candidate = current;
      candidate_runs = 1;
      continue;
    }
    candidate_runs++;
    if (candidate_runs < 3) continue;  // require 3 × 20 ms = 60 ms stable

    // Confirmed transition.
    last_steady = current;
    candidate_runs = 0;

    if (current) continue;  // we only act on press (LOW), not release

    // Pressed: engage ack iff at least one slot is currently blocked.
    // Either way, the press counts as user activity for screensaver purposes.
    g_last_activity_us = esp_timer_get_time();
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      bool any_blocked = false;
      for (int i = 0; i < TOTAL_SLOTS; i++) {
        if (g_clients[i].pinned && g_clients[i].state == Status::BLOCKED) {
          any_blocked = true;
          break;
        }
      }
      if (any_blocked && !g_blocked_ack_active) {
        g_blocked_ack_active = true;
        ESP_LOGI(TAG, "ack: silencing blocked alarm");
      }
      xSemaphoreGive(g_mutex);
    }
  }
}

// ======================================================================
// Text rendering — all drawing is viewport-relative.
// ======================================================================

static void draw_glyph(Hub75Driver &d, const Viewport &vp, int16_t x, int16_t y,
                       const uint8_t rows[7], uint8_t r, uint8_t g, uint8_t b) {
  for (int row = 0; row < FONT_H; row++) {
    const int16_t local_y = y + row;
    if (local_y < 0 || local_y >= vp.h) continue;
    const uint8_t bits = rows[row];
    for (int col = 0; col < FONT_W; col++) {
      const int16_t local_x = x + col;
      if (local_x < 0 || local_x >= vp.w) continue;
      if (bits & (1 << (FONT_W - 1 - col))) {
        d.set_pixel(static_cast<uint16_t>(vp.x0 + local_x),
                    static_cast<uint16_t>(vp.y0 + local_y), r, g, b);
      }
    }
  }
}

static void draw_text(Hub75Driver &d, const Viewport &vp, int16_t x, int16_t y,
                      const char *s, uint8_t r, uint8_t g, uint8_t b) {
  while (*s) {
    const uint8_t c = static_cast<uint8_t>(*s);
    if (c < 128) draw_glyph(d, vp, x, y, FONT_5X7[c], r, g, b);
    x += FONT_STRIDE;
    s++;
  }
}

// Scaled glyph drawer — each set bit becomes a scale × scale block. Used by
// the identify display to paint panel IDs large enough to read across a room.
static void draw_glyph_scaled(Hub75Driver &d, const Viewport &vp,
                              int16_t x, int16_t y, const uint8_t rows[7],
                              int scale, uint8_t r, uint8_t g, uint8_t b) {
  for (int row = 0; row < FONT_H; row++) {
    const uint8_t bits = rows[row];
    for (int col = 0; col < FONT_W; col++) {
      if (!(bits & (1 << (FONT_W - 1 - col)))) continue;
      for (int dy = 0; dy < scale; dy++) {
        const int16_t local_y = y + row * scale + dy;
        if (local_y < 0 || local_y >= vp.h) continue;
        for (int dx = 0; dx < scale; dx++) {
          const int16_t local_x = x + col * scale + dx;
          if (local_x < 0 || local_x >= vp.w) continue;
          d.set_pixel(static_cast<uint16_t>(vp.x0 + local_x),
                      static_cast<uint16_t>(vp.y0 + local_y), r, g, b);
        }
      }
    }
  }
}

static int text_width_px(const char *s) {
  int n = 0;
  while (*s++) n++;
  return n > 0 ? n * FONT_STRIDE - 1 : 0;
}

// ======================================================================
// State visuals.
// ======================================================================

struct Rgb { uint8_t r, g, b; };

static Rgb status_color_bright(Status s) {
  switch (s) {
    case Status::IDLE:       return {30,  180, 40 };
    case Status::WORKING:    return {240, 160, 10 };
    case Status::THINKING:   return {210, 120, 30 };
    case Status::BLOCKED:    return {240, 40,  40 };
    case Status::COMPACTING: return {30,  180, 200};
    case Status::ERROR:      return {255, 30,  30 };
    default:                 return {80,  80,  220};
  }
}

static Rgb status_color_dim(Status s) {
  switch (s) {
    case Status::IDLE:       return {4,  28, 6  };
    case Status::WORKING:    return {50, 32, 2  };
    case Status::THINKING:   return {40, 24, 4  };
    case Status::BLOCKED:    return {60, 8,  8  };
    case Status::COMPACTING: return {4,  28, 32 };
    case Status::ERROR:      return {80, 6,  6  };
    default:                 return {14, 14, 40 };
  }
}

static float border_intensity(Status s, float t_since) {
  switch (s) {
    case Status::IDLE: {
      const float phase = fmodf(t_since, 6.0f) / 6.0f;
      return 0.9f + 0.1f * sinf(phase * 2.0f * static_cast<float>(M_PI));
    }
    case Status::WORKING: {
      const float phase = fmodf(t_since, 1.2f) / 1.2f;
      return 0.675f + 0.325f * sinf(phase * 2.0f * static_cast<float>(M_PI));
    }
    case Status::THINKING: {
      const float phase = fmodf(t_since, 3.0f) / 3.0f;
      return 0.775f + 0.225f * sinf(phase * 2.0f * static_cast<float>(M_PI));
    }
    case Status::BLOCKED: {
      const float phase = fmodf(t_since, 0.6f) / 0.6f;
      return phase < 0.6f ? 1.0f : 0.08f;
    }
    case Status::COMPACTING: {
      return 0.6f;
    }
    case Status::ERROR: {
      if (t_since < 3.0f) {
        const float phase = fmodf(t_since, 0.25f) / 0.25f;
        return phase < 0.5f ? 1.0f : 0.15f;
      }
      return 1.0f;
    }
    default: {
      const float phase = fmodf(t_since, 4.0f) / 4.0f;
      return 0.5f + 0.5f * sinf(phase * 2.0f * static_cast<float>(M_PI));
    }
  }
}

static void render_border(Hub75Driver &d, const Viewport &vp, Status s, float t_since,
                          bool acked, bool screensaver) {
  uint8_t r, g, b;
  if (screensaver) {
    // Heavy dim, no animation. Override any state-specific behavior so a
    // BLOCKED slot stops strobing and an IDLE slot stops breathing — the
    // whole point of the screensaver is "leave me alone."
    const Rgb bright = status_color_bright(s);
    r = static_cast<uint8_t>(bright.r * SCREENSAVER_DIM_NUM / SCREENSAVER_DIM_DEN);
    g = static_cast<uint8_t>(bright.g * SCREENSAVER_DIM_NUM / SCREENSAVER_DIM_DEN);
    b = static_cast<uint8_t>(bright.b * SCREENSAVER_DIM_NUM / SCREENSAVER_DIM_DEN);
  } else if (acked) {
    // Calm steady glow, no strobe. Uses the bright color at low intensity
    // so it's visible-but-quiet, distinct from the alarming variant.
    const Rgb bright = status_color_bright(s);
    constexpr float ACK_BORDER_INTENSITY = 0.4f;
    r = static_cast<uint8_t>(bright.r * ACK_BORDER_INTENSITY);
    g = static_cast<uint8_t>(bright.g * ACK_BORDER_INTENSITY);
    b = static_cast<uint8_t>(bright.b * ACK_BORDER_INTENSITY);
  } else {
    const Rgb dim = status_color_dim(s);
    const float i = border_intensity(s, t_since);
    r = static_cast<uint8_t>(dim.r * i);
    g = static_cast<uint8_t>(dim.g * i);
    b = static_cast<uint8_t>(dim.b * i);
  }

  for (int x = 0; x < vp.w; x++) {
    d.set_pixel(vp.x0 + x, vp.y0,             r, g, b);
    d.set_pixel(vp.x0 + x, vp.y0 + vp.h - 1,  r, g, b);
  }
  for (int y = 1; y < vp.h - 1; y++) {
    d.set_pixel(vp.x0,             vp.y0 + y, r, g, b);
    d.set_pixel(vp.x0 + vp.w - 1,  vp.y0 + y, r, g, b);
  }

  if (s == Status::COMPACTING) {
    constexpr float SWEEP_PERIOD = 2.0f;
    const float phase = fmodf(t_since, SWEEP_PERIOD) / SWEEP_PERIOD;
    const int head_x = static_cast<int>(phase * vp.w);
    const Rgb bright = status_color_bright(s);
    for (int dx = -1; dx <= 1; dx++) {
      const int x = head_x + dx;
      if (x < 0 || x >= vp.w) continue;
      const uint8_t scale = (dx == 0) ? 255 : 96;
      const uint8_t rr = static_cast<uint8_t>(bright.r * scale / 255);
      const uint8_t gg = static_cast<uint8_t>(bright.g * scale / 255);
      const uint8_t bb = static_cast<uint8_t>(bright.b * scale / 255);
      d.set_pixel(vp.x0 + x, vp.y0,             rr, gg, bb);
      d.set_pixel(vp.x0 + x, vp.y0 + vp.h - 1,  rr, gg, bb);
    }
  }
}

// Auto-fall-back to shortened names when the viewport is too narrow for the
// full word. Keeps the renderer oblivious to panel-vs-half layout.
static const char *state_text_for(Status s, int width) {
  const char *longer  = nullptr;
  const char *shorter = nullptr;
  switch (s) {
    case Status::IDLE:       longer = "IDLE";       shorter = "IDLE"; break;
    case Status::WORKING:    longer = "WORKING";    shorter = "WRK";  break;
    case Status::THINKING:   longer = "THINKING";   shorter = "THNK"; break;
    case Status::BLOCKED:    longer = "BLOCKED";    shorter = "BLK";  break;
    case Status::COMPACTING: longer = "COMPACTING"; shorter = "CMPT"; break;
    case Status::ERROR:      longer = "ERROR";      shorter = "ERR";  break;
    default: return "";
  }
  const int long_w = text_width_px(longer);
  return (long_w <= width - 2) ? longer : shorter;  // -2 leaves breathing room from border
}

// Scale a state color by the ack mute factor and/or the screensaver dim
// factor. Screensaver dominates when both are active — its scale is much
// heavier, so chaining ack on top would just clip down a few more bits
// for no useful contrast.
static constexpr int ACK_NUM = 7;   // 70% of bright
static constexpr int ACK_DEN = 10;
static inline Rgb apply_dim(Rgb c, bool acked, bool screensaver) {
  int num = ACK_DEN;  // start at 1.0 in ACK units
  int den = ACK_DEN;
  if (screensaver) {
    num = SCREENSAVER_DIM_NUM;
    den = SCREENSAVER_DIM_DEN;
  } else if (acked) {
    num = ACK_NUM;
    den = ACK_DEN;
  }
  return {
    static_cast<uint8_t>(c.r * num / den),
    static_cast<uint8_t>(c.g * num / den),
    static_cast<uint8_t>(c.b * num / den),
  };
}

static void render_state_text(Hub75Driver &d, const Viewport &vp, Status s,
                              bool acked, bool screensaver) {
  const Rgb c = apply_dim(status_color_bright(s), acked, screensaver);
  const int16_t y = (vp.h - FONT_H) / 2;

  if (s == Status::UNKNOWN) {
    // "…" — three single pixels along the vertical centerline.
    const int spacing = (vp.w >= 48) ? 4 : 3;
    const int16_t cx = vp.w / 2;
    for (int i = -1; i <= 1; i++) {
      d.set_pixel(vp.x0 + cx + i * spacing, vp.y0 + y + FONT_H - 1, c.r, c.g, c.b);
    }
    return;
  }

  const char *text = state_text_for(s, vp.w);
  const int tw = text_width_px(text);
  const int16_t x = (vp.w - tw) / 2;
  draw_text(d, vp, x, y, text, c.r, c.g, c.b);
}

// Task-progress bar. Mirrors the subagent bar geometry but lives at the
// top of the viewport (just inside the top border). Completed tasks are
// rendered as bright cells, active tasks (pending + in_progress) as dim
// cells of the same state color. If the total exceeds MAX_RECTS, the
// rightmost cell becomes OVERFLOW_COLOR — same convention as the
// subagent bar — to signal "more is going on than what's shown."
static void render_task_bar(Hub75Driver &d, const Viewport &vp, Status s,
                            int active, int completed, bool acked, bool screensaver) {
  if (active <= 0 && completed <= 0) return;
  if (active < 0)    active = 0;
  if (completed < 0) completed = 0;

  const bool narrow = (vp.w < 48);
  const int rect_w  = narrow ? 2 : 4;
  const int rect_h  = narrow ? 2 : 3;
  const int stride  = rect_w + 1;
  constexpr int MAX_RECTS = 10;
  constexpr Rgb OVERFLOW_COLOR_RAW = {200, 50, 50};

  const int total = active + completed;
  const int rect_count = (total > MAX_RECTS) ? MAX_RECTS : total;
  const int total_w = MAX_RECTS * stride - 1;
  const int16_t x0 = (vp.w - total_w) / 2;
  const int16_t y0 = 1 + 1;  // 1 row of border, 1 row of gap

  const Rgb bright = apply_dim(status_color_bright(s), acked, screensaver);
  const Rgb dim    = {static_cast<uint8_t>(bright.r * 35 / 100),
                      static_cast<uint8_t>(bright.g * 35 / 100),
                      static_cast<uint8_t>(bright.b * 35 / 100)};
  const Rgb overflow = apply_dim(OVERFLOW_COLOR_RAW, acked, screensaver);

  for (int i = 0; i < rect_count; i++) {
    const int16_t cx = x0 + i * stride;
    const bool is_overflow = (total > MAX_RECTS && i == rect_count - 1);
    const Rgb c = is_overflow ? overflow : (i < completed ? bright : dim);
    for (int dy = 0; dy < rect_h; dy++) {
      for (int dx = 0; dx < rect_w; dx++) {
        d.set_pixel(vp.x0 + cx + dx, vp.y0 + y0 + dy, c.r, c.g, c.b);
      }
    }
  }
}

// Subagent-count bar. Cells scale down for narrow viewports so the bar
// always fits with at least 2 px of side margin.
static void render_subagent_bar(Hub75Driver &d, const Viewport &vp, Status s, int count,
                                bool acked, bool screensaver) {
  if (count <= 0) return;

  const bool narrow = (vp.w < 48);
  const int rect_w  = narrow ? 2 : 4;
  const int rect_h  = narrow ? 2 : 3;
  const int stride  = rect_w + 1;
  constexpr int MAX_RECTS = 10;
  constexpr Rgb OVERFLOW_COLOR_RAW = {200, 50, 50};

  const int rect_count = (count > MAX_RECTS) ? MAX_RECTS : count;
  const int total_w = MAX_RECTS * stride - 1;
  const int16_t x0 = (vp.w - total_w) / 2;
  const int16_t y0 = vp.h - 1 - 1 - rect_h;
  const Rgb base     = apply_dim(status_color_bright(s), acked, screensaver);
  const Rgb overflow = apply_dim(OVERFLOW_COLOR_RAW, acked, screensaver);

  for (int i = 0; i < rect_count; i++) {
    const int16_t cx = x0 + i * stride;
    const bool is_overflow = (count > MAX_RECTS && i == rect_count - 1);
    const Rgb c = is_overflow ? overflow : base;
    for (int dy = 0; dy < rect_h; dy++) {
      for (int dx = 0; dx < rect_w; dx++) {
        d.set_pixel(vp.x0 + cx + dx, vp.y0 + y0 + dy, c.r, c.g, c.b);
      }
    }
  }
}

// Per-slot RX pulse — single pixel just inside top-right border, decays
// over 250 ms after the slot's last received line.
static void render_rx_flash(Hub75Driver &d, const Viewport &vp, Status s,
                            int64_t now_us, int64_t last_line_us) {
  if (last_line_us == 0) return;
  constexpr int64_t FLASH_US = 250'000;
  const int64_t since = now_us - last_line_us;
  if (since >= FLASH_US) return;
  const float i = 1.0f - static_cast<float>(since) / static_cast<float>(FLASH_US);
  const Rgb c = status_color_bright(s);
  d.set_pixel(vp.x0 + vp.w - 3, vp.y0 + 1,
              static_cast<uint8_t>(c.r * i),
              static_cast<uint8_t>(c.g * i),
              static_cast<uint8_t>(c.b * i));
}

// Placeholder for the opposite half of a panel when only one half is pinned.
// A single dim dot near the vertical center, quiet enough not to compete.
static void render_unpinned_placeholder(Hub75Driver &d, const Viewport &vp) {
  const int16_t cx = vp.w / 2;
  const int16_t cy = vp.h / 2;
  d.set_pixel(vp.x0 + cx, vp.y0 + cy, 12, 12, 18);
}

// Identify display — paint each owned panel's ID large and centered, in a
// distinct magenta so the user immediately knows this isn't a state screen.
// Auto-scales the glyph(s) to fit the viewport with a 2-px margin.
static void render_identify(Hub75Driver &d) {
  constexpr Rgb IDENTIFY_COLOR = {200, 30, 200};

  for (uint8_t p = 0; p < g_panel_count; p++) {
    const Viewport vp = viewport_for(p, SLOT_FULL);

    char id[4];
    snprintf(id, sizeof(id), "%d", g_first_id + p);
    const int len = static_cast<int>(strlen(id));

    // Maximum scale that still fits the string + 2 px margin both axes.
    int scale = 1;
    while (true) {
      const int s = scale + 1;
      const int total_w = (len * FONT_STRIDE - 1) * s;
      const int total_h = FONT_H * s;
      if (total_w > vp.w - 2 || total_h > vp.h - 2) break;
      scale = s;
    }

    const int total_w = (len * FONT_STRIDE - 1) * scale;
    const int total_h = FONT_H * scale;
    const int16_t x0  = (vp.w - total_w) / 2;
    const int16_t y0  = (vp.h - total_h) / 2;

    for (int i = 0; i < len; i++) {
      const uint8_t c = static_cast<uint8_t>(id[i]);
      if (c < 128) {
        draw_glyph_scaled(d, vp,
                          x0 + i * FONT_STRIDE * scale, y0,
                          FONT_5X7[c], scale,
                          IDENTIFY_COLOR.r, IDENTIFY_COLOR.g, IDENTIFY_COLOR.b);
      }
    }
  }
}

// Bridge-heartbeat indicator — single pixel at the bottom-left of the
// framebuffer. Green if a byte from the host arrived recently; red-orange
// otherwise. The bridge sends a ping every ~5s by default (see
// cc-status-bridge/docs/FIRMWARE.md), so anything older than ~10s
// implies the bridge has gone away — crashed, host asleep, or cable
// unplugged. We deliberately don't use usb_serial_jtag_is_connected()
// here: it tracks USB SOF packets, which keep arriving as long as a
// powered, awake host has the device enumerated, and so it can't
// distinguish "bridge talking to me" from "USB still plugged in but
// the bridge process is dead."
static constexpr int64_t CONN_FRESH_THRESHOLD_US = 10'000'000;

static void render_conn_dot(Hub75Driver &d, int64_t now_us) {
  const int16_t h = static_cast<int16_t>(d.get_height());
  const int64_t last = g_last_rx_us;
  const bool fresh = (last > 0) && (now_us - last < CONN_FRESH_THRESHOLD_US);
  if (fresh) d.set_pixel(2, h - 2, 0, 80, 16);
  else       d.set_pixel(2, h - 2, 80, 8, 0);
}

// ======================================================================
// Per-slot rendering
// ======================================================================

static void render_slot(Hub75Driver &d, const Viewport &vp,
                        const ClientSnapshot &cs, int64_t now_us,
                        bool ack_active, bool screensaver) {
  const Status state = cs.pinned ? cs.state : Status::UNKNOWN;
  const int64_t changed_at = cs.pinned ? cs.state_changed_at_us : 0;
  const float t_since =
      (changed_at > 0) ? static_cast<float>(now_us - changed_at) / 1'000'000.0f
                       : static_cast<float>(now_us) / 1'000'000.0f;
  // Per-slot ack: only this slot's rendering is muted, and only when
  // it's actually in BLOCKED. Other slots in working/idle/etc. on the
  // same ESP render normally.
  const bool acked = ack_active && cs.pinned && cs.state == Status::BLOCKED;

  render_border(d, vp, state, t_since, acked, screensaver);
  render_state_text(d, vp, state, acked, screensaver);
  if (cs.pinned) {
    render_task_bar(d, vp, state, cs.tasks_active, cs.tasks_completed, acked, screensaver);
    render_subagent_bar(d, vp, state, cs.subagent_count, acked, screensaver);
    // RX pulse fires on every byte received — the bridge pings every ~5s,
    // so leaving it on in screensaver mode would be a periodic blink that
    // defeats the dim. Skip it.
    if (!screensaver) {
      render_rx_flash(d, vp, state, now_us, cs.last_line_at_us);
    }
  }
}

// ======================================================================
// Entry point
// ======================================================================

extern "C" void app_main() {
  // Hold HUB75 OE high as early as possible to suppress the boot flash.
  // The lasting fix is a 10 kΩ OE→3V3 hardware pull-up, but this firmware
  // assist trims the residual flash window after app_main starts.
  gpio_config_t oe_cfg = {};
  oe_cfg.pin_bit_mask = 1ULL << BOARD_PINS.oe;
  oe_cfg.mode = GPIO_MODE_OUTPUT;
  oe_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  oe_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  oe_cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&oe_cfg);
  gpio_set_level(static_cast<gpio_num_t>(BOARD_PINS.oe), 1);

  init_font5x7();
  // Firmware version is built from version.txt + (for non-release
  // builds) a 6-char commit SHA appended as "+abc123". CI release
  // workflow strips the SHA suffix; local dev/canary/smoke builds
  // keep it. See CMakeLists.txt and MAINTENANCE.md.
  const esp_app_desc_t *desc = esp_app_get_description();
  ESP_LOGI(TAG, "firmware: %s  board: %s", desc->version, BOARD_LABEL);

  // NVS init before anything that reads the saved config. If the partition
  // is corrupt or a version mismatch (rare on a dev board), wipe and retry
  // once — the bridge will re-send `configure` after reboot anyway.
  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_err);

  const bool nvs_loaded = nvs_load_config();
  ESP_LOGI(TAG, "config source: %s (panels=%d first_id=%d %ux%u)",
           nvs_loaded ? "NVS" : "defaults",
           g_panel_count, g_first_id, g_panel_width, g_panel_height);

  g_mutex        = xSemaphoreCreateMutex();
  g_driver_mutex = xSemaphoreCreateMutex();

  g_driver = new Hub75Driver(build_hub_cfg());
  if (!g_driver->begin()) {
    ESP_LOGE(TAG, "Hub75Driver::begin() failed on initial boot");
    return;
  }

  if (!usb_serial_jtag_is_driver_installed()) {
    usb_serial_jtag_driver_config_t usj = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usj.rx_buffer_size = 1024;
    usj.tx_buffer_size = 256;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usj));
  }

  // 6 KB stack: handle_ping → send_pong's worst-case frame is ~3.5 KB
  // (large local pong buffer + snapshot of g_clients + serial_reader's own
  // line/chunk buffers). 4 KB was right at the edge and overflowed under
  // the v1.2 full-table pong format.
  xTaskCreate(serial_reader_task, "serial_rx", 6144, nullptr, 5, nullptr);
  xTaskCreate(ack_button_task,    "ack_btn",   3072, nullptr, 4, nullptr);

  ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));

  ESP_LOGI(TAG, "setup complete; entering render loop");

  ClientSnapshot local[TOTAL_SLOTS];

  while (true) {
    esp_task_wdt_reset();

    // Hold the driver mutex for the whole frame so a concurrent configure
    // can't tear down the driver mid-render. Bounded wait so we skip the
    // frame (rather than WDT-panic) if a configure takes unusually long.
    if (xSemaphoreTake(g_driver_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(33));
      continue;
    }

    g_driver->clear();

    const int64_t now = esp_timer_get_time();

    int64_t identify_until = 0;
    bool ack_active = false;
    bool any_pinned = false;
    bool any_active_state = false;  // pinned slot in a non-{IDLE,BLOCKED} state
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      memcpy(local, g_clients, sizeof(local));
      identify_until = g_identify_until_us;

      // Auto-clear the ESP-wide ack when no slot is currently blocked.
      // Walk the live g_clients here (not the local copy) so the cleared
      // flag is visible to the next button press without a frame of delay.
      // Same walk also feeds the screensaver eligibility check.
      bool any_blocked = false;
      for (int i = 0; i < TOTAL_SLOTS; i++) {
        if (!g_clients[i].pinned) continue;
        any_pinned = true;
        const Status st = g_clients[i].state;
        if (st == Status::BLOCKED) any_blocked = true;
        if (st != Status::IDLE && st != Status::BLOCKED) any_active_state = true;
      }
      if (!any_blocked && g_blocked_ack_active) {
        g_blocked_ack_active = false;
      }
      ack_active = g_blocked_ack_active;

      xSemaphoreGive(g_mutex);
    }

    // Screensaver: kick in once everything pinned has been quiet (IDLE or
    // BLOCKED) for SCREENSAVER_TIMEOUT_US, where "quiet" is reset by any
    // state change or ack press. Skipped entirely when nothing is pinned
    // — discoverability matters more than power savings on a fresh boot.
    const bool screensaver_eligible = any_pinned && !any_active_state;
    const bool screensaver = screensaver_eligible
        && (now - g_last_activity_us) > SCREENSAVER_TIMEOUT_US;

    if (now < identify_until) {
      // Identify display owns the panel until the deadline. Snapshots still
      // accumulate via apply_state_update; they're just not rendered yet.
      render_identify(*g_driver);
      render_conn_dot(*g_driver, now);
      g_driver->flip_buffer();
      xSemaphoreGive(g_driver_mutex);
      vTaskDelay(pdMS_TO_TICKS(33));
      continue;
    }

    for (uint8_t p = 0; p < g_panel_count; p++) {
      const ClientSnapshot &full = local[slot_index(p, SLOT_FULL)];
      const ClientSnapshot &half_a = local[slot_index(p, SLOT_HALF_A)];
      const ClientSnapshot &half_b = local[slot_index(p, SLOT_HALF_B)];

      if (full.pinned) {
        render_slot(*g_driver, viewport_for(p, SLOT_FULL), full, now, ack_active, screensaver);
      } else if (half_a.pinned || half_b.pinned) {
        // Split mode: render each half. Unpinned sibling gets a placeholder.
        if (half_a.pinned) {
          render_slot(*g_driver, viewport_for(p, SLOT_HALF_A), half_a, now, ack_active, screensaver);
        } else {
          render_unpinned_placeholder(*g_driver, viewport_for(p, SLOT_HALF_A));
        }
        if (half_b.pinned) {
          render_slot(*g_driver, viewport_for(p, SLOT_HALF_B), half_b, now, ack_active, screensaver);
        } else {
          render_unpinned_placeholder(*g_driver, viewport_for(p, SLOT_HALF_B));
        }
      } else {
        // Nothing pinned on this panel — show UNKNOWN across its full viewport.
        render_slot(*g_driver, viewport_for(p, SLOT_FULL), full, now, ack_active, screensaver);
      }
    }

    render_conn_dot(*g_driver, now);

    g_driver->flip_buffer();

    xSemaphoreGive(g_driver_mutex);

    vTaskDelay(pdMS_TO_TICKS(33));
  }
}
