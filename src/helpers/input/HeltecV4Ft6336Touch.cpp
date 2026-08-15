#if defined(HAS_HELTEC_V4_FT6336_TOUCH) && defined(ESP32)

#include "HeltecV4CapTouch.h"
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <helpers/ui/MomentaryButton.h>

#ifndef PIN_TOUCH_SDA
  #error PIN_TOUCH_SDA required for HAS_HELTEC_V4_FT6336_TOUCH
#endif
#ifndef PIN_TOUCH_SCL
  #error PIN_TOUCH_SCL required for HAS_HELTEC_V4_FT6336_TOUCH
#endif
#ifndef PIN_TOUCH_RST
  #define PIN_TOUCH_RST -1
#endif
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT -1
#endif
#ifndef TOUCH_I2C_ADDR
  #define TOUCH_I2C_ADDR 0x38
#endif

#ifndef TOUCH_SCREEN_WIDTH
  #define TOUCH_SCREEN_WIDTH 320
#endif
#ifndef TOUCH_SCREEN_HEIGHT
  #define TOUCH_SCREEN_HEIGHT 480
#endif

#ifndef HELTEC_V4_TOUCH_LONG_MS
  #define HELTEC_V4_TOUCH_LONG_MS 1000
#endif
#ifndef HELTEC_V4_TOUCH_LONG_MOVE_MAX
  #define HELTEC_V4_TOUCH_LONG_MOVE_MAX 18
#endif
#ifndef HELTEC_V4_TOUCH_SWIPE_MIN
  #define HELTEC_V4_TOUCH_SWIPE_MIN 36
#endif
#ifndef HELTEC_V4_TOUCH_SWIPE_INVERT
  #define HELTEC_V4_TOUCH_SWIPE_INVERT 0
#endif

static TwoWire s_touch_wire(1);
static bool s_init_ok = false;
static bool s_given_up = false;
static int s_retries = 0;
static char s_scan_str[96] = "";

static volatile bool s_touch_down = false;
static volatile uint16_t s_live_x = 0;
static volatile uint16_t s_live_y = 0;

static uint8_t s_ui_rotation = 0;
static uint8_t s_point_rotation = 0;

static bool s_tap_pending = false;
static uint16_t s_tap_x = 0;
static uint16_t s_tap_y = 0;

static bool s_swipe_pending = false;
static int8_t s_swipe_x = 0;
static int8_t s_swipe_y = 0;
static bool s_swiping_now = false;

static uint16_t s_down_x = 0;
static uint16_t s_down_y = 0;
static uint32_t s_down_ms = 0;

static TaskHandle_t s_poll_task = nullptr;
static volatile bool s_async = false;
static uint32_t s_period_ms = 8;

static bool ft_read_touch(uint16_t* x, uint16_t* y) {
  if (!x || !y) return false;

  s_touch_wire.beginTransmission(TOUCH_I2C_ADDR);
  s_touch_wire.write(0x02);
  if (s_touch_wire.endTransmission() != 0) return false;

  if (s_touch_wire.requestFrom((uint16_t)TOUCH_I2C_ADDR, (uint8_t)5, (uint8_t)true) < 5) {
    return false;
  }

  const uint8_t touches = s_touch_wire.read() & 0x0F;
  const uint8_t xh = s_touch_wire.read();
  const uint8_t xl = s_touch_wire.read();
  const uint8_t yh = s_touch_wire.read();
  const uint8_t yl = s_touch_wire.read();

  if (touches == 0) return false;

  uint16_t rx = ((uint16_t)(xh & 0x0F) << 8) | xl;
  uint16_t ry = ((uint16_t)(yh & 0x0F) << 8) | yl;

  if (rx >= TOUCH_SCREEN_WIDTH) rx = TOUCH_SCREEN_WIDTH - 1;
  if (ry >= TOUCH_SCREEN_HEIGHT) ry = TOUCH_SCREEN_HEIGHT - 1;

  *x = rx;
  *y = ry;
  return true;
}

static void apply_rotation(uint16_t* x, uint16_t* y, uint8_t rot) {
  if (!x || !y) return;
  uint16_t tx = *x;
  uint16_t ty = *y;

  switch (rot & 3) {
    case 1:
      *x = TOUCH_SCREEN_WIDTH - 1 - ty;
      *y = tx;
      break;
    case 2:
      *x = TOUCH_SCREEN_WIDTH - 1 - tx;
      *y = TOUCH_SCREEN_HEIGHT - 1 - ty;
      break;
    case 3:
      *x = ty;
      *y = TOUCH_SCREEN_HEIGHT - 1 - tx;
      break;
    default:
      break;
  }
}

bool heltecV4CapTouchBegin() {
  if (s_init_ok) return true;
  if (s_given_up) return false;

  ++s_retries;
  if (s_retries > 3) {
    s_given_up = true;
    snprintf(s_scan_str, sizeof s_scan_str, "FT6336 give-up");
    return false;
  }

  if (PIN_TOUCH_RST >= 0) {
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(40);
  }
  if (PIN_TOUCH_INT >= 0) {
    pinMode(PIN_TOUCH_INT, INPUT);
  }

  s_touch_wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
  s_touch_wire.setClock(400000);
  s_touch_wire.setTimeOut(20);

  s_touch_wire.beginTransmission(TOUCH_I2C_ADDR);
  if (s_touch_wire.endTransmission() != 0) {
    snprintf(s_scan_str, sizeof s_scan_str, "no ACK 0x%02X", TOUCH_I2C_ADDR);
    return false;
  }

  snprintf(s_scan_str, sizeof s_scan_str, "FT6336 OK @0x%02X", TOUCH_I2C_ADDR);
  s_init_ok = true;
  return true;
}

int heltecV4CapTouchCheck() {
  if (!s_init_ok && !heltecV4CapTouchBegin()) {
    return BUTTON_EVENT_NONE;
  }

  uint16_t x = 0;
  uint16_t y = 0;
  const bool down = ft_read_touch(&x, &y);

  if (down) {
    apply_rotation(&x, &y, s_point_rotation);
    s_live_x = x;
    s_live_y = y;
    s_touch_down = true;

    if (s_down_ms == 0) {
      s_down_ms = millis();
      s_down_x = x;
      s_down_y = y;
      return BUTTON_EVENT_NONE;
    }

    const int dx = (int)x - (int)s_down_x;
    const int dy = (int)y - (int)s_down_y;
    if (!s_swiping_now && (abs(dx) >= HELTEC_V4_TOUCH_SWIPE_MIN || abs(dy) >= HELTEC_V4_TOUCH_SWIPE_MIN)) {
      s_swiping_now = true;
      s_swipe_pending = true;
      s_swipe_x = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
      s_swipe_y = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);
      if (HELTEC_V4_TOUCH_SWIPE_INVERT) {
        s_swipe_x = -s_swipe_x;
        s_swipe_y = -s_swipe_y;
      }
      return BUTTON_EVENT_NONE;
    }

    return BUTTON_EVENT_NONE;
  }

  if (!s_touch_down) {
    return BUTTON_EVENT_NONE;
  }

  s_touch_down = false;
  const uint32_t held = millis() - s_down_ms;
  const int dx = (int)s_live_x - (int)s_down_x;
  const int dy = (int)s_live_y - (int)s_down_y;

  if (!s_swiping_now && held < HELTEC_V4_TOUCH_LONG_MS && abs(dx) <= HELTEC_V4_TOUCH_LONG_MOVE_MAX && abs(dy) <= HELTEC_V4_TOUCH_LONG_MOVE_MAX) {
    s_tap_pending = true;
    s_tap_x = s_live_x;
    s_tap_y = s_live_y;
  }

  s_swiping_now = false;
  s_down_ms = 0;
  return BUTTON_EVENT_NONE;
}

bool heltecV4CapTouchPopTap(uint16_t* x, uint16_t* y) {
  if (!s_tap_pending) return false;
  s_tap_pending = false;
  if (x) *x = s_tap_x;
  if (y) *y = s_tap_y;
  return true;
}

bool heltecV4CapTouchGetLive(uint16_t* x, uint16_t* y) {
  if (!s_touch_down) return false;
  if (x) *x = s_live_x;
  if (y) *y = s_live_y;
  return true;
}

bool heltecV4CapTouchPopSwipe(int8_t* x_dir, int8_t* y_dir) {
  if (!s_swipe_pending) return false;
  s_swipe_pending = false;
  if (x_dir) *x_dir = s_swipe_x;
  if (y_dir) *y_dir = s_swipe_y;
  return true;
}

static void touchPollTask(void* arg) {
  (void)arg;
  while (true) {
    heltecV4CapTouchCheck();
    vTaskDelay(pdMS_TO_TICKS(s_period_ms));
  }
}

bool heltecV4CapTouchStartBackgroundPoll(uint32_t period_ms) {
  if (s_poll_task) return true;
  s_period_ms = (period_ms == 0) ? 8 : period_ms;
  const BaseType_t ok = xTaskCreatePinnedToCore(
    touchPollTask,
    "ft6336touch",
    4096,
    nullptr,
    1,
    &s_poll_task,
    0
  );
  s_async = (ok == pdPASS);
  return s_async;
}

bool heltecV4CapTouchIsAsyncPolling() {
  return s_async;
}

bool heltecV4CapTouchIsSwiping() {
  return s_swiping_now;
}

void heltecV4CapTouchSetRotation(uint8_t r) {
  s_ui_rotation = r & 3;
  (void)s_ui_rotation;
}

void heltecV4CapTouchSetPointRotation(uint8_t r) {
  s_point_rotation = r & 3;
}

const char* heltecV4CapTouchDebug() {
  return s_scan_str;
}

void heltecV4CapTouchGetRaw(uint16_t* rx, uint16_t* ry) {
  if (rx) *rx = s_live_x;
  if (ry) *ry = s_live_y;
}

#endif
