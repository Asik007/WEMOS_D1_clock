#include <RTClib.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFiManager.h>
#include <time.h>
#include <sys/time.h>
#include <coredecls.h>      // settimeofday_cb()
#include <sntp.h>
#include <MD_MAX72xx.h>
#include <MD_Parola.h>

// ---------- Hardware SPI Definitions ----------
#define PIN_CS D8   // Standard CS pin for ESP8266 Hardware SPI

// ---------- Display hardware configuration ----------
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 8
#define BRIGHTNESS 1

// ---------- NTP settings ----------
#define NTP_SERVER "pool.ntp.org"
#define TZ_STRING "CST6CDT,M3.2.0,M11.1.0"   // US Central, DST handled automatically
#define MIN_VALID_EPOCH 1700000000           // earlier than this = time not set yet

// SNTP resync interval (the core calls this function). Was 12 hours in the original.
uint32_t sntp_update_delay_MS_rfc_not_less_than_15000() {
  return 12UL * 60UL * 60UL * 1000UL;
}

RTC_DS3231 rtc;
bool rtcOk = false;

// ---------- Custom Monospace Digits (5x8 - Taller & Wider) ----------
uint8_t monoDigits[10][6] = {
  {5, 0x7E, 0x81, 0x81, 0x81, 0x7E}, // 0
  {5, 0x00, 0x82, 0xFF, 0x80, 0x00}, // 1
  {5, 0xC2, 0xA1, 0x91, 0x91, 0x8E}, // 2
  {5, 0x42, 0x81, 0x89, 0x89, 0x76}, // 3
  {5, 0x38, 0x24, 0x22, 0xFF, 0x20}, // 4
  {5, 0x4F, 0x89, 0x89, 0x89, 0x71}, // 5
  {5, 0x7E, 0x89, 0x89, 0x89, 0x72}, // 6
  {5, 0x01, 0x01, 0xE1, 0x19, 0x07}, // 7
  {5, 0x76, 0x89, 0x89, 0x89, 0x76}, // 8
  {5, 0x4E, 0x91, 0x91, 0x91, 0x7E}  // 9
};

// Zero-width character assigned to '`' to create a 1-pixel shift
uint8_t shiftSpace[1] = {0};

// ---------- Time Buffer ----------
char timeString[30];   // holds "`HH:MM:SS.mmm"

// ---------- Global objects ----------
MD_Parola P = MD_Parola(HARDWARE_TYPE, PIN_CS, MAX_DEVICES);

// ---------- Timing Variables ----------
unsigned long lastTimeUpdate = 0;
const unsigned long REFRESH_RATE = 11;   // ms of time between frames
volatile bool sntpSynced = false;        // set by the SNTP callback, handled in loop()

// Called by the core whenever SNTP sets the system time. Just raise a flag.
void onSntpSync() {
  sntpSynced = true;
}

// Build the display string from ONE read of the system clock (UTC -> local via TZ).
void formatTime() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);

  if (tv.tv_sec < MIN_VALID_EPOCH) {
    snprintf(timeString, sizeof(timeString), "`--:--:--.---");   // no valid time yet
    return;
  }
  struct tm t;
  time_t sec = tv.tv_sec;
  localtime_r(&sec, &t);
  snprintf(timeString, sizeof(timeString), "`%02d:%02d:%02d.%03d",
           t.tm_hour, t.tm_min, t.tm_sec, (int)(tv.tv_usec / 1000));
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nStarting Dedicated Live NTP Clock");

  // ---- Initialize display ----
  P.begin();
  P.setIntensity(BRIGHTNESS);

  // ---- Override digits with custom monospace font ----
  for (int i = 0; i < 10; i++) {
    P.addChar('0' + i, monoDigits[i]);
  }
  // Backtick (`) becomes a 0-width spacer (shifts subsequent characters right)
  P.addChar('`', shiftSpace);

  P.setTextAlignment(PA_LEFT);   // set once; print() uses it every time
  P.displayClear();
  P.print("Init");

  // ---- Start from the RTC (stored as UTC) so there is a time even without Wi-Fi ----
  rtcOk = rtc.begin();
  if (!rtcOk) {
    Serial.println("RTC not found - continuing with NTP only.");
  } else if (rtc.lostPower()) {
    Serial.println("RTC lost power - waiting for NTP.");
  } else {
    struct timeval tv = { (time_t)rtc.now().unixtime(), 0 };
    settimeofday(&tv, nullptr);
    Serial.println("Clock seeded from RTC.");
  }

  // ---- Start SNTP (runs in the background once Wi-Fi is up) ----
  settimeofday_cb(onSntpSync);               // registered after seeding, so only SNTP triggers it
  configTime(TZ_STRING, NTP_SERVER);

  // ---- Connect WiFi ----
  WiFiManager wifiManager;
  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalTimeout(120);   // don't block forever if the router is down
  if (!wifiManager.autoConnect("ClockAP")) {
    Serial.println("WiFi not connected - running from RTC.");
  } else {
    Serial.println("WiFi connected, IP: " + WiFi.localIP().toString());
  }
  WiFi.mode(WIFI_STA);   // make sure the config AP is off

  // ---- Init Screen ----
  formatTime();
  P.print(timeString);
}

void loop() {
  // 1. After a successful SNTP sync, copy the new time to the RTC (UTC).
  if (sntpSynced) {
    sntpSynced = false;
    if (rtcOk) {
      rtc.adjust(DateTime((uint32_t)time(nullptr)));
    }
    Serial.println("SNTP sync OK; RTC updated.");
  }

  // 2. Update display at a controlled framerate
  if (millis() - lastTimeUpdate >= REFRESH_RATE) {
    lastTimeUpdate = millis();
    formatTime();
    P.print(timeString);   // draws immediately; no animation, so no displayAnimate() needed
  }
}