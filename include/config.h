#pragma once

#define FW_VERSION          "1.0.0"

#define PIN_TRIAC           4
#define PIN_ZC              5
#define PIN_RELAY           6
#define PIN_CURRENT         1
#define PIN_BUTTON          9

#define RELAY_ACTIVE_HIGH   1

#define USE_CURRENT_SENSOR  1
#define CURRENT_MV_ZERO     2500.0f
#define CURRENT_MV_PER_A    66.7f
#define CURRENT_DIVIDER     1.0f

#define AP_SSID_PREFIX      "SoftStart"
#define AP_PASS             "12345678"

#define DEF_START_PCT       40
#define DEF_RAMP_TIME       5
#define DEF_SOFT_STOP       0
#define DEF_MAX_CURRENT     0

#define RELAY_ON_DELAY_MS   800

#define AUTO_START_ON_MAINS 1
#define RELAY_ZC_TIMEOUT_MS 100
#define RELAY_OFF_GUARD_MS  50
