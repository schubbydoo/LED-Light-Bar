// Stand-ins for the Arduino core and the ESP32 radio, so the REAL bridge.ino
// can be compiled and driven on a PC. They mirror only what the sketch calls,
// with the signatures the existing sketches already use against core 3.3.x.
//
// What this CANNOT prove: that those signatures match the real library. Only a
// compile on the real core does that (./flash.sh bridge bridge).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>

struct __FlashStringHelper;
#define F(s) (reinterpret_cast<const __FlashStringHelper*>(s))
#define HEX 16

// ---- time: the test owns the clock; delay() advances it -------------------
extern uint32_t g_now;
inline uint32_t millis() { return g_now; }
inline void delay(uint32_t ms) { g_now += ms; }
inline void setCpuFrequencyMhz(int) {}

// ---- Serial: output captured, input injected ------------------------------
struct FakeSerial {
  std::string out, in;
  size_t inPos = 0;
  void begin(long) {}
  void setTxTimeoutMs(int) {}
  explicit operator bool() const { return true; }
  void feed(const std::string& s) { in += s; }
  int available() { return (int)(in.size() - inPos); }
  int read() { return available() ? (unsigned char)in[inPos++] : -1; }
  size_t write(uint8_t c) { out += (char)c; return 1; }
  void print(const char* s) { out += s; }
  void print(const __FlashStringHelper* s) { out += reinterpret_cast<const char*>(s); }
  void print(char c) { out += c; }
  void print(int v)           { out += std::to_string(v); }
  void print(unsigned v)      { out += std::to_string(v); }
  void print(long v)          { out += std::to_string(v); }
  void print(unsigned long v) { out += std::to_string(v); }
  void print(unsigned v, int base) {
    char b[16]; snprintf(b, sizeof b, base == 16 ? "%X" : "%u", v); out += b;
  }
  void println() { out += "\n"; }
  template <class T> void println(T v) { print(v); out += "\n"; }
};
extern FakeSerial Serial;

// ---- WiFi ------------------------------------------------------------------
#define WIFI_STA 1
#define WIFI_SECOND_CHAN_NONE 0
struct FakeWiFi {
  void mode(int) {}
  void disconnect() {}
  const char* macAddress() { return "F0:9E:9E:B2:3D:38"; }
};
extern FakeWiFi WiFi;
inline int esp_wifi_set_channel(uint8_t, int) { return 0; }

// ---- ESP-NOW ---------------------------------------------------------------
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_NOW_MAX_DATA_LEN 250
typedef enum { ESP_NOW_SEND_SUCCESS = 0, ESP_NOW_SEND_FAIL } esp_now_send_status_t;
struct esp_now_send_info_t { uint8_t des_addr[6]; };
struct esp_now_recv_info_t { uint8_t* src_addr; uint8_t* des_addr; };
struct esp_now_peer_info_t { uint8_t peer_addr[6]; uint8_t channel; bool encrypt; };
typedef void (*esp_now_send_cb_t)(const esp_now_send_info_t*, esp_now_send_status_t);
typedef void (*esp_now_recv_cb_t)(const esp_now_recv_info_t*, const uint8_t*, int);

// What the "radio" saw. The test inspects this to check WHAT went to WHOM, WHEN.
struct SentPacket { uint8_t mac[6]; std::string data; uint32_t at; };
extern std::vector<SentPacket> g_sent;
extern esp_now_send_cb_t g_sendCb;
extern esp_now_recv_cb_t g_recvCb;
extern std::vector<std::vector<uint8_t>> g_peersAdded;

inline esp_err_t esp_now_init() { return ESP_OK; }
inline esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb) { g_sendCb = cb; return ESP_OK; }
inline esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb) { g_recvCb = cb; return ESP_OK; }
inline esp_err_t esp_now_add_peer(const esp_now_peer_info_t* p) {
  g_peersAdded.push_back(std::vector<uint8_t>(p->peer_addr, p->peer_addr + 6));
  return ESP_OK;
}
inline esp_err_t esp_now_send(const uint8_t* mac, const uint8_t* data, size_t len) {
  SentPacket s; memcpy(s.mac, mac, 6); s.data.assign((const char*)data, len); s.at = g_now;
  g_sent.push_back(s);
  return ESP_OK;
}
