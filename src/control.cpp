// FW-VERSION: V12.3-common-pilot — control.cpp
#include <Arduino.h>
#include <math.h>

#include "control.h"
#include "vfd.h"
#include "sensors.h"
#include "logging.h"
#include "profiles.h"   // g_common, g_boilerMode, MODE_*

// ================= ШНЕК: внутренние переменные =================
static uint16_t aug_Ton   = 0;  // мс
static uint16_t aug_Toff  = 0;  // мс
static uint32_t aug_tMark = 0;
static bool     aug_isOn  = false;

// ================= Клампы минимальных длительностей =================
static inline float clampTon(float s)  { const float mn = g_common.aug_min_ton_ms  / 1000.0f; return (s < mn) ? mn : s; }
static inline float clampToff(float s) { const float mn = g_common.aug_min_toff_ms / 1000.0f; return (s < mn) ? mn : s; }

// ================= Расчёт duty для шнека по доле P [0..1] =================
void auger_update(float P)
{
  if (isnan(P) || P <= 0.0f) {
    // «почти выкл.» — соблюдая минимумы
    aug_Ton = (uint16_t)g_common.aug_min_ton_ms;
    uint16_t pause = g_common.aug_min_toff_ms;
    if (pause < 3000) pause = 3000;  // чтобы реально была длинная пауза
    aug_Toff = pause;
    return;
  }
  if (P > 1.0f) P = 1.0f;

  const float Tcycle = 5.0f; // сек
  float ton_s  = clampTon (Tcycle * P);
  float toff_s = clampToff(Tcycle - ton_s);
  if (toff_s < (g_common.aug_min_toff_ms/1000.0f)) {
    toff_s = (g_common.aug_min_toff_ms/1000.0f);
  }

  aug_Ton  = (uint16_t)roundf(ton_s  * 1000.0f);
  aug_Toff = (uint16_t)roundf(toff_s * 1000.0f);
}

// ================= Коммутация шнека (под свои реле/ПЧ) =================
static void auger_hwSwitch(bool /*on*/)
{
  // Привяжи сюда свою реальную команду на шнек (реле/вход ПЧ).
  // setAuger(on);
}

static void auger_tick()
{
  const uint32_t now = millis();
  if (!aug_isOn) {
    if (now - aug_tMark >= aug_Toff) {
      aug_isOn  = true;
      aug_tMark = now;
      auger_hwSwitch(true);
    }
  } else {
    if (now - aug_tMark >= aug_Ton) {
      aug_isOn  = false;
      aug_tMark = now;
      auger_hwSwitch(false);
    }
  }
}

// ================= Инициализация управления (реализация controlInit) =================
void controlInit()
{
  // безопасное состояние старта
  applySpeed(0);                // вентилятор/ПЧ в ноль
  aug_isOn  = false;
  aug_Ton   = (uint16_t)g_common.aug_min_ton_ms;
  aug_Toff  = (uint16_t)g_common.aug_min_toff_ms;
  aug_tMark = millis();
  auger_hwSwitch(false);

  logMsg("controlInit()");
}

// ================= Основной цикл управления =================
void controlTick()
{
  // Защита по максимальной температуре подачи
  {
    float ts = getTsupply();
    if (!isnan(ts) && ts > g_common.T_supply_max) {
      applySpeed(0);
      g_boilerMode = MODE_ALARM;
      logMsg("АВАРИЯ: превышение T подачи");
    }
  }

  switch (g_boilerMode)
  {
    case MODE_PILOT: {
      // ФИТИЛЬ: вентилятор — минимальная ступень; шнек — слабая подача
      float fan_pct = g_common.fan_steps[0];
      uint8_t best = 0; float bd = 1e9f;
      for (uint8_t i=0; i<8; ++i) { // PC-00..07
        float d = fabsf(g_common.fan_steps[i] - fan_pct);
        if (d < bd) { bd = d; best = i; }
      }
      const uint8_t desired = best + 1; // applySpeed: 1..8
      if (g_speed_mode != desired && millis() - g_lastSpeedChangeMs > 2000) {
        applySpeed(desired);
      }

      float Ppilot = g_common.auger_steps10[0] / 100.0f;
      if (Ppilot < 0) Ppilot = 0; if (Ppilot > 1) Ppilot = 1;
      auger_update(Ppilot);
    } break;

    case MODE_OFF:
      applySpeed(0);
      auger_update(0);
      break;

    case MODE_MANUAL:
      // Оставь твою реализацию ручного режима здесь
      break;

    #ifdef MODE_IGNITION
    case MODE_IGNITION:
      // Оставь твою реализацию розжига
      break;
    #endif

    case MODE_ALARM:
      applySpeed(0);
      auger_update(0);
      break;

    default:
      break;
  }

  // тикаем шнек
  auger_tick();
}
