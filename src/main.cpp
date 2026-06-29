// Do Baskets Dream — node firmware (single image for every node).
//
// One conductor broadcasts a clock beacon over ESP-NOW; performers lock to it and
// render against synced time. A performer that misses a beacon keeps free-running
// on its last known offset and re-locks on the next one — a dropped packet causes
// at most slight drift, never a blackout.
//
// Every board runs THIS SAME binary. Role (conductor/performer) is a runtime
// value stored in NVS, default performer, set once over serial with `role …`.
// So you flash one image everywhere, then provision role/id/pos per board; each
// node then boots into its role unattended (important for battery field nodes).
//
// The sync logic lives in include/sync.h (dependency-free, unit-tested); this
// file is the on-device glue: radio, LEDs, NVS config, serial, and the render loop.

#include <Arduino.h>
#include <NeoPixelBus.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_mac.h>  // esp_read_mac / ESP_MAC_WIFI_STA

#include "config.h"
#include "beacon.h"
#include "identity.h"
#include "patterns.h"
#include "roster.h"
#include "sync.h"

// 16x SK6812 RGBW ring. RMT channel 0 with SK6812 timing.
NeoPixelBus<NeoGrbwFeature, NeoEsp32Rmt0Sk6812Method> strip(LED_COUNT, LED_PIN);

static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---- Node config in NVS (role + identity; set once over serial) --------------
static Preferences  g_prefs;
static NodeIdentity g_id   = {0, 0.0f, 0.0f};
static uint8_t      g_role = DEFAULT_ROLE;
static uint8_t      g_mac[6] = {0};  // this node's WiFi STA MAC — stable identity

static inline bool isConductor() { return g_role == ROLE_CONDUCTOR; }

// Format a MAC into a caller-supplied 18-byte buffer ("AA:BB:CC:DD:EE:FF").
static const char* macStr(const uint8_t mac[6], char out[18]) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  return out;
}

static void configLoad() {
  g_prefs.begin("node", /*readonly*/ true);
  g_id.id = g_prefs.getUShort("id", 0);
  g_id.x = g_prefs.getFloat("x", 0.0f);
  g_id.y = g_prefs.getFloat("y", 0.0f);
  g_role = g_prefs.getUChar("role", DEFAULT_ROLE);
  g_prefs.end();
}

static void identitySave() {
  g_prefs.begin("node", /*readonly*/ false);
  g_prefs.putUShort("id", g_id.id);
  g_prefs.putFloat("x", g_id.x);
  g_prefs.putFloat("y", g_id.y);
  g_prefs.end();
}

static void roleSave() {
  g_prefs.begin("node", /*readonly*/ false);
  g_prefs.putUChar("role", g_role);
  g_prefs.end();
}

// ---- Sync state --------------------------------------------------------------
// Written from the ESP-NOW recv callback, read from loop(). Guarded by a spinlock
// so 64-bit fields can't tear across the two contexts.
static SyncState g_sync;
static BeaconMsg g_beacon = {{BEACON_MAGIC, PROTO_VERSION, MSG_BEACON},
                             /*epoch*/ 0, patterns::SWEEP, /*brightness*/ 48,
                             /*palette*/ 0, {0, 0, 0, 0}, 0};
static portMUX_TYPE g_sync_mux = portMUX_INITIALIZER_UNLOCKED;

// Performer -> conductor registration state. The conductor's MAC is learned from
// the recv-info of an incoming beacon (written in the recv callback under
// g_sync_mux); the actual peer-add + unicast happens from loop(), since doing
// radio work inside the recv callback is unsafe.
static uint8_t  g_conductor_mac[6] = {0};
static bool     g_have_conductor = false;
static bool     g_conductor_peer_added = false;
static int64_t  g_next_register_us = 0;

// Conductor roster: every node that has registered, keyed on MAC. The logic lives
// in roster.h (host-tested); here we hold one instance and a spinlock, since it is
// written from the recv callback (MSG_REGISTER) and read from loop() (the `roster`
// command). The lock wraps each access, mirroring g_sync_mux around syncOnBeacon.
static Roster       g_roster;
static portMUX_TYPE g_roster_mux = portMUX_INITIALIZER_UNLOCKED;

static inline int64_t now_us() { return esp_timer_get_time(); }

// ---- Pattern config in NVS (the broadcast recipe; tweaked live over serial) ---
// Defined here, *after* g_beacon, so it can touch it — configLoad() above can't
// (it's defined before g_beacon). Only the conductor's recipe drives the field,
// but every node persists/restores it so a conductor survives a power-cycle with
// its tuning intact, and this seeds the show-program storage later.
static void patternConfigLoad() {
  g_prefs.begin("node", /*readonly*/ true);
  g_beacon.pattern_id = g_prefs.getUShort("pat", patterns::SWEEP);
  g_beacon.brightness = g_prefs.getUChar("bri", 48);
  g_beacon.params[0] = g_prefs.getUShort("p0", 0);
  g_beacon.params[1] = g_prefs.getUShort("p1", 0);
  g_beacon.params[2] = g_prefs.getUShort("p2", 0);
  g_beacon.params[3] = g_prefs.getUShort("p3", 0);
  g_prefs.end();
}

static void patternConfigSave() {
  g_prefs.begin("node", /*readonly*/ false);
  g_prefs.putUShort("pat", g_beacon.pattern_id);
  g_prefs.putUChar("bri", g_beacon.brightness);
  g_prefs.putUShort("p0", g_beacon.params[0]);
  g_prefs.putUShort("p1", g_beacon.params[1]);
  g_prefs.putUShort("p2", g_beacon.params[2]);
  g_prefs.putUShort("p3", g_beacon.params[3]);
  g_prefs.end();
}

// ---- ESP-NOW receive ---------------------------------------------------------
// Registered on every node. Validates the common header, then dispatches on the
// message type. The recv callback signature changed in Arduino-ESP32 3.x —
// support both, and grab the sender MAC, which we need for bidirectional traffic.
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* src = info->src_addr;
#else
void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  const uint8_t* src = mac;
#endif
  if (len < (int)sizeof(MsgHeader)) return;
  MsgHeader hdr;
  memcpy(&hdr, data, sizeof(hdr));
  if (hdr.magic != BEACON_MAGIC || hdr.version != PROTO_VERSION) return;

  switch (hdr.type) {
    case MSG_BEACON: {
      if (isConductor()) return;  // a conductor follows no one
      if (len != (int)sizeof(BeaconMsg)) return;
      BeaconMsg b;
      memcpy(&b, data, sizeof(b));
      int64_t local = now_us();
      portENTER_CRITICAL(&g_sync_mux);
      syncOnBeacon(g_sync, b.epoch_us, b.seq, local);
      g_beacon = b;
      memcpy(g_conductor_mac, src, 6);  // remember who to register with
      g_have_conductor = true;
      portEXIT_CRITICAL(&g_sync_mux);
      break;
    }
    case MSG_REGISTER: {
      if (!isConductor()) return;  // only the conductor keeps a roster
      if (len != (int)sizeof(RegisterMsg)) return;
      RegisterMsg r;
      memcpy(&r, data, sizeof(r));
      portENTER_CRITICAL(&g_roster_mux);
      rosterUpsert(g_roster, r.mac, r.id, r.fw, now_us());
      portEXIT_CRITICAL(&g_roster_mux);
      break;
    }
    default:
      break;
  }
}

// ---- Radio setup -------------------------------------------------------------
static void radioBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();  // we never join an AP; just need the STA interface up

  // Pin the channel explicitly so every node agrees without scanning.
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  // Modem-sleep is a battery-budget requirement (brief: don't-break list). It is
  // the default in STA mode; set it explicitly so it can't silently regress.
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  esp_now_register_recv_cb(onRecv);  // every node; dispatches on message type
}

static uint32_t g_tx_seq = 0;
static void broadcastBeacon() {
  BeaconMsg b = g_beacon;
  b.hdr.magic = BEACON_MAGIC;
  b.hdr.version = PROTO_VERSION;
  b.hdr.type = MSG_BEACON;
  b.epoch_us = now_us();
  b.seq = g_tx_seq++;
  esp_now_send(BROADCAST_ADDR, (const uint8_t*)&b, sizeof(b));
}

// Performer: announce ourselves to the conductor we've heard, periodically so the
// roster self-heals after a conductor restart. Peer-add + send happen here (in
// loop context), never in the recv callback.
static void maybeRegister(int64_t t) {
  if (isConductor()) return;

  bool have;
  uint8_t cmac[6];
  portENTER_CRITICAL(&g_sync_mux);
  have = g_have_conductor;
  if (have) memcpy(cmac, g_conductor_mac, 6);
  portEXIT_CRITICAL(&g_sync_mux);
  if (!have || t < g_next_register_us) return;
  g_next_register_us = t + REGISTER_INTERVAL_US;

  if (!g_conductor_peer_added) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, cmac, 6);
    peer.channel = WIFI_CHANNEL;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) == ESP_OK) g_conductor_peer_added = true;
  }
  if (!g_conductor_peer_added) return;

  RegisterMsg r = {{BEACON_MAGIC, PROTO_VERSION, MSG_REGISTER}, {0}, g_id.id,
                   PROTO_VERSION};
  memcpy(r.mac, g_mac, 6);
  esp_now_send(cmac, (const uint8_t*)&r, sizeof(r));
}

// ---- Diagnostics -------------------------------------------------------------
// One-line status per second so boards can be range-walked to find where sync
// drops.
static void printDiag() {
  if (isConductor()) {
    Serial.printf("[conductor] t=%lld us  seq=%lu  pat=%u  bri=%u\n",
                  (long long)now_us(), (unsigned long)g_tx_seq, g_beacon.pattern_id,
                  g_beacon.brightness);
    return;
  }
  SyncState s;
  portENTER_CRITICAL(&g_sync_mux);
  s = g_sync;
  portEXIT_CRITICAL(&g_sync_mux);

  int64_t t = now_us();
  bool stale = syncIsStale(s, t, BEACON_STALE_US);
  int64_t age = beaconAge(s, t);
  Serial.printf(
      "[performer] %s  offset=%lld us  last_beacon=%lld ms ago  rx=%lu  gaps=%lu  seq=%lu\n",
      stale ? "FREE-RUN" : "LOCKED  ", (long long)s.offset_us,
      (long long)(age < 0 ? -1 : age / 1000), (unsigned long)s.beacons_rx,
      (unsigned long)s.seq_gaps, (unsigned long)s.last_seq);
}

// ---- Serial command interface ------------------------------------------------
// Flash identical firmware to every board, then provision each over serial:
//   info                 print role + identity (incl. MAC) + pattern state
//   roster               (conductor) list nodes that have registered (MAC/id/fw)
//   role <conductor|performer>   set this node's role and save to NVS
//   id <n>               set this node's id and save to NVS
//   pos <x> <y>          set this node's (x,y) coordinate and save to NVS
// Pattern controls (only the conductor's take effect field-wide; it broadcasts):
//   pattern <n>          0 = uniform pulse, 1 = rainbow drift, 2 = sweep
//   bri <n>              brightness 0-255
//   param <i> <v>        params[i] (i=0..3): sweep period_ms / wavelength*100
static void printInfo() {
  char mac[18];
  Serial.printf("role=%s  id=%u  mac=%s  x=%.2f  y=%.2f\n",
                isConductor() ? "CONDUCTOR" : "PERFORMER", g_id.id,
                macStr(g_mac, mac), g_id.x, g_id.y);
  Serial.printf("  pattern=%u  bri=%u  params=[%u %u %u %u]\n", g_beacon.pattern_id,
                g_beacon.brightness, g_beacon.params[0], g_beacon.params[1],
                g_beacon.params[2], g_beacon.params[3]);
}

// Conductor-only: print the roster of nodes that have registered. Snapshots the
// shared array under the spinlock, then prints outside it.
static void printRoster() {
  if (!isConductor()) {
    Serial.println("(roster lives on the conductor)");
    return;
  }
  Roster snap;
  portENTER_CRITICAL(&g_roster_mux);
  snap = g_roster;
  portEXIT_CRITICAL(&g_roster_mux);

  int64_t t = now_us();
  Serial.printf("roster: %u node(s)\n", snap.count);
  for (uint8_t i = 0; i < snap.count; i++) {
    char mac[18];
    Serial.printf("  [%u] %s  id=%u  fw=%u  last_seen=%lld ms ago\n", i,
                  macStr(snap.entries[i].mac, mac), snap.entries[i].id,
                  snap.entries[i].fw,
                  (long long)((t - snap.entries[i].last_us) / 1000));
  }
}

static void handleCommand(char* line) {
  char* cmd = strtok(line, " \t");
  if (!cmd) return;

  if (!strcmp(cmd, "info")) {
    printInfo();
  } else if (!strcmp(cmd, "roster")) {
    printRoster();
  } else if (!strcmp(cmd, "role")) {
    char* a = strtok(nullptr, " \t");
    if (a) {
      if (!strcmp(a, "conductor") || !strcmp(a, "1")) g_role = ROLE_CONDUCTOR;
      else if (!strcmp(a, "performer") || !strcmp(a, "0")) g_role = ROLE_PERFORMER;
      else { Serial.println("? role conductor|performer"); return; }
      roleSave();
      printInfo();
    }
  } else if (!strcmp(cmd, "id")) {
    char* a = strtok(nullptr, " \t");
    if (a) { g_id.id = (uint16_t)atoi(a); identitySave(); printInfo(); }
  } else if (!strcmp(cmd, "pos")) {
    char* ax = strtok(nullptr, " \t");
    char* ay = strtok(nullptr, " \t");
    if (ax && ay) { g_id.x = atof(ax); g_id.y = atof(ay); identitySave(); printInfo(); }
  } else if (!strcmp(cmd, "pattern")) {
    char* a = strtok(nullptr, " \t");
    if (a) { g_beacon.pattern_id = (uint16_t)atoi(a); patternConfigSave(); printInfo(); }
  } else if (!strcmp(cmd, "bri")) {
    char* a = strtok(nullptr, " \t");
    if (a) { g_beacon.brightness = (uint8_t)atoi(a); patternConfigSave(); printInfo(); }
  } else if (!strcmp(cmd, "param")) {
    char* ai = strtok(nullptr, " \t");
    char* av = strtok(nullptr, " \t");
    if (ai && av) {
      int i = atoi(ai);
      if (i >= 0 && i < 4) {
        g_beacon.params[i] = (uint16_t)atoi(av);
        patternConfigSave();
        printInfo();
      }
    }
  } else {
    Serial.printf("? unknown command: %s\n", cmd);
  }
}

// Accumulate a newline-terminated command from serial without blocking.
static void pollSerialCommands() {
  static char buf[64];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) { buf[len] = '\0'; handleCommand(buf); len = 0; }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

// ---- Arduino entry points ----------------------------------------------------
void setup() {
  setCpuFrequencyMhz(CPU_FREQ_MHZ);
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nDo Baskets Dream — channel %u\n", WIFI_CHANNEL);

  configLoad();
  patternConfigLoad();
  esp_read_mac(g_mac, ESP_MAC_WIFI_STA);  // stable identity, read from efuse
  rosterInit(g_roster);
  if (!identityProvisioned(g_id))
    Serial.println("  (unprovisioned — set 'role …', 'id <n>', 'pos <x> <y>')");
  printInfo();

  syncInit(g_sync);

#if HEARTBEAT_LED
  pinMode(HEARTBEAT_LED_PIN, OUTPUT);
#endif

  strip.Begin();
  strip.Show();  // clear

  radioBegin();
}

void loop() {
  int64_t t = now_us();

  pollSerialCommands();

  if (isConductor()) {
    static int64_t next_beacon = 0;
    if (t >= next_beacon) {
      broadcastBeacon();
      next_beacon = t + BEACON_INTERVAL_US;
    }
  } else {
    maybeRegister(t);  // announce ourselves to the conductor periodically
  }

  // Snapshot sync + beacon together, then render against the right clock.
  SyncState s;
  BeaconMsg b;
  portENTER_CRITICAL(&g_sync_mux);
  s = g_sync;
  b = g_beacon;
  portEXIT_CRITICAL(&g_sync_mux);

  // Conductor renders against its own clock; a performer against synced time
  // (which free-runs on the last offset when no beacon arrives).
  int64_t render_us = isConductor() ? t : syncedTime(s, t);
  patterns::render(strip, b, render_us, g_id.x, g_id.y);
  strip.Show();

#if HEARTBEAT_LED
  bool on = pmath::heartbeatOn(render_us, HEARTBEAT_HALF_US);
  digitalWrite(HEARTBEAT_LED_PIN, (on != HEARTBEAT_ACTIVE_LOW) ? HIGH : LOW);
#endif

  static int64_t next_diag = 0;
  if (t >= next_diag) {
    printDiag();
    next_diag = t + DIAG_INTERVAL_US;
  }

  delay(16);  // ~60 fps render cap; keeps the CPU mostly idle for modem-sleep
}
