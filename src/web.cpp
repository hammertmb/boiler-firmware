// FW-VERSION: V10.1-UI-ProfilesRework-4 — web.cpp (UI only; файл из последнего архива)
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

#include "web.h"
#include "vfd.h"
#include "profiles.h"
#include "sensors.h"
#include "logging.h"
#include "control.h"
#include "pins.h"
#include <Preferences.h>   // <-- добавлено

// Единственное определение HTTP-сервера (в web.h — extern WebServer server;)
WebServer server(80);

static const char* FW_VER = "V10.1-UI-ProfilesRework-4";

// ========== Утилиты JSON ==========
static inline String q(const String& s){ String r="\""; r+=s; r+="\""; return r; }
static inline String jn(const char* k, const String& v, bool last=false){ String s=q(k); s+=':'; s+=v; if(!last) s+=','; return s; }

// ========== /api/status ==========
static String jsonStatus(){
  String j="{";
  const float feedKgph = getCurrentFuelFeedKgPerHour();
  j+=jn("fw", q(FW_VER));
  j+=jn("speed", String(g_speed_mode));
  j+=jn("feed_kgph", String(feedKgph, 2));
  // relays
  j+="\"relays\":{";
    j+=jn("run",  String(relayIsOn(PIN_RELAY_RUN) ? "true":"false"));
    j+=jn("ms1",  String(relayIsOn(PIN_RELAY_MS1) ? "true":"false"));
    j+=jn("ms2",  String(relayIsOn(PIN_RELAY_MS2) ? "true":"false"));
    j+=jn("ms3",  String(relayIsOn(PIN_RELAY_MS3) ? "true":"false"), true);
  j+="},";

  // VFD statuses (по умолчанию false — при наличии реального состояния заменить на актуальные флаги)
  j += "\"vfd\":{";
    j += jn("v1_conn", String("false"));
    j += jn("v1_on",   String("false"));
    j += jn("v2_conn", String("false"));
    j += jn("v2_on",   String("false"));
    j += jn("v3_conn", String("false"), true);
  j += "},";

  // temperatures
  float t1=getTsupply(), t2=getTreturn(), tk=getTflue();
  j+="\"temp\":{";
    j+=jn("ds1", isnan(t1)? "null":String(t1,1));
    j+=jn("ds2", isnan(t2)? "null":String(t2,1));
    j+=jn("stack", isnan(tk)? "null":String(tk,1), true);
  j+="},";

  // boiler
  j+="\"boiler\":{";
    j+=jn("mode", String((int)g_boilerMode));
    j+=jn("modeName", q(modeRu(g_boilerMode)));
    j+=jn("setpoint", String(g_cfg.T_set,1), true);
  j+="}";

  j+="}";
  return j;
}

// ========== Примитивные парсеры (без сторонних JSON-библиотек) ==========
static bool parseF(const String& b,const char* k,float& out){
  int p=b.indexOf(String("\"")+k+"\""); if(p<0) return false;
  int c=b.indexOf(':',p); if(c<0) return false;
  int e=b.indexOf(',',c); if(e<0) e=b.indexOf('}',c);
  out=b.substring(c+1,e).toFloat(); return true;
}
static bool parseU32(const String& b,const char* k,uint32_t& out){
  int p=b.indexOf(String("\"")+k+"\""); if(p<0) return false;
  int c=b.indexOf(':',p); if(c<0) return false;
  int e=b.indexOf(',',c); if(e<0) e=b.indexOf('}',c);
  out=(uint32_t)strtoul(b.substring(c+1,e).c_str(),nullptr,10); return true;
}

// ========== API ==========
static void handleStatus(){ server.sendHeader("Cache-Control","no-store"); server.send(200,"application/json; charset=utf-8", jsonStatus()); }
static void handleLogs(){ server.sendHeader("Cache-Control","no-store"); server.send(200,"text/plain; charset=utf-8", logDump()); }

static void handleCmd(){ // POST {"speed":0..8}
  if(!server.hasArg("plain")){ server.send(400,"text/plain; charset=utf-8","Нет тела"); return; }
  String body = server.arg("plain");
  Serial.println(String("[WEB] handleCmd body: ")+body);

  int p = body.indexOf("\"speed\"");
  if(p < 0){ server.send(400,"text/plain; charset=utf-8","Нет 'speed'"); return; }
  int c = body.indexOf(':', p);
  int sp = body.substring(c+1).toInt();
  if(sp < 0) sp = 0; if(sp > 8) sp = 8;

  // Защита от слишком частых переключений
  if (sp!=0 && g_speed_mode!=0 && millis()-g_lastSpeedChangeMs<2000){
    Serial.println("[WEB] handleCmd: rejected - too frequent");
    server.send(202,"application/json; charset=utf-8", jsonStatus()); return;
  }

  Serial.println(String("[WEB] handleCmd: applySpeed ") + String(sp));
  applySpeed((uint8_t)sp);
  g_speed_mode = (uint8_t)sp;
  g_lastSpeedChangeMs = millis();

  // Принудительно синхронизируем реле RUN с текущей скоростью
  // Если логика реле у вас инвертирована — поменяйте HIGH/LOW
  // Синхронизируем RUN через setRelay (учитывает инверсию)
  setRelay(PIN_RELAY_RUN, (g_speed_mode > 0));
  Serial.println(String("[HW] RUN relay -> ") + (g_speed_mode>0 ? "ON":"OFF"));

  // Если есть реальная функция управления VFD - вызывать здесь:
  // vfdSetSpeed(sp);

  server.send(200,"application/json; charset=utf-8", jsonStatus());
}

static void handleBoiler(){ // POST {"mode":0..5, "setpoint":<C>}
  if(!server.hasArg("plain")){ server.send(400,"text/plain; charset=utf-8","Нет тела"); return; }
  String b=server.arg("plain");
  int pm=b.indexOf("\"mode\""); if(pm<0){ server.send(400,"text/plain; charset=utf-8","Нет 'mode'"); return; }
  int cm=b.indexOf(':',pm); int m=b.substring(cm+1).toInt(); if(m<0)m=0; if(m>5)m=5; // UI авторежим — отдельная галка, бэкенд остаётся 0..5

  g_boilerMode=(uint8_t)m; applyProfileToCfg(g_boilerMode);
  int ps=b.indexOf("\"setpoint\""); if(ps>=0){ int cs=b.indexOf(':',ps); g_cfg.T_set=b.substring(cs+1).toFloat(); }
  if (g_boilerMode==MODE_OFF) applySpeed(0);
  server.send(200,"application/json; charset=utf-8", jsonStatus());
}

// ——— Профили: отдаём/принимаем Tflue_low/high ———
static String jsonProfileSlim(const ModeProfile& p, uint8_t /*mode*/){
  String j="{";
  j+=jn("Tflue_low", String(p.Tflue_low,1));
  j+=jn("Tflue_high", String(p.Tflue_high,1), true);
  j+="}";
  return j;
}

static void handleProfileGet(){ // /api/profile?m=X
  if(!server.hasArg("m")){ server.send(400,"text/plain; charset=utf-8","Нужно m=0..5"); return; }
  int m=server.arg("m").toInt(); if(m<0||m>5){ server.send(400,"text/plain; charset=utf-8","Неверный режим"); return; }
  server.send(200,"application/json; charset=utf-8", jsonProfileSlim(profileOf(m),(uint8_t)m));
}

static void handleProfilePost(){ // POST (сохраняем Tflue_low/high для выбранного режима)
  if(!server.hasArg("m")){ server.send(400,"text/plain; charset=utf-8","Нужно m=0..5"); return; }
  int m=server.arg("m").toInt(); if(m<0||m>5){ server.send(400,"text/plain; charset=utf-8","Неверный режим"); return; }
  if(server.hasArg("reset")){ profileFactoryReset(m); profilesSaveToNVS(); server.send(200,"application/json; charset=utf-8", jsonProfileSlim(profileOf(m),(uint8_t)m)); return; }
  if(!server.hasArg("plain")){ server.send(400,"text/plain; charset=utf-8","Нет тела"); return; }

  String b=server.arg("plain");
  ModeProfile p = profileOf(m);
  parseF(b,"Tflue_low",p.Tflue_low);
  parseF(b,"Tflue_high",p.Tflue_high);

  updateProfile(m,p);
  profilesSaveToNVS();
  if(m==g_boilerMode) applyProfileToCfg(m);
  server.send(200,"application/json; charset=utf-8", jsonProfileSlim(profileOf(m),(uint8_t)m));
}

// ====== Common (общие) — хранение в NVS через Preferences ======
static String jsonCommonDefault(){
  String j = "{";
  j += jn("amin_on", String(1000));
  j += jn("amin_off", String(2000));
  j += jn("tmax", String(90,1));
  j += jn("tflue_high", String(220,1));
  j += jn("kp", String(1.2,3));
  j += jn("ki", String(0.01,4));
  j += jn("q100", String(10.0f,3));
  j += jn("kmat", String(1.0f,3));
  // fan, fan2, aug10 arrays (по 10 значений 10..100)
  j += "\"fan\":["; for(int i=0;i<10;i++){ if(i) j += ','; j += String((i+1)*10); } j += "],";
  j += "\"fan2\":["; for(int i=0;i<10;i++){ if(i) j += ','; j += String((i+1)*10); } j += "],";
  j += "\"aug10\":["; for(int i=0;i<10;i++){ if(i) j += ','; j += String((i+1)*10); } j += "]";
  j += "}";
  return j;
}

static void handleCommonGet(){
  Preferences prefs;
  prefs.begin("boiler", true); // readonly
  String s = prefs.getString("common", "");
  prefs.end();
  if (s.length()>0){
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json; charset=utf-8", s);
  } else {
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json; charset=utf-8", jsonCommonDefault());
  }
}

static void handleCommonPost(){
  if(!server.hasArg("plain")){ server.send(400,"text/plain; charset=utf-8","Нет тела"); return; }
  String body = server.arg("plain");
  // Небольшая валидация: должно быть JSON-объектом
  if(!body.startsWith("{") || !body.endsWith("}")){ server.send(400,"text/plain; charset=utf-8","Неверный JSON"); return; }
  Preferences prefs;
  prefs.begin("boiler", false); // writable
  prefs.putString("common", body);
  prefs.end();
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"application/json; charset=utf-8", body);
}

// ========== INDEX.HTML (UI) ==========
static String INDEX_HTML(){
  String h;
  h.reserve(42000);
  h += F("<!doctype html><html lang='ru'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>Управление котлом</title><style>");
  h += F(":root{--bg:#0f1115;--panel:#161a22;--text:#e6eaf2;--muted:#9aa3b2;--ok:#21c07a;--warn:#ffb020;--err:#ff4d4f;--btn:#222837;--btnA:#2a3245}");
  h += F("html,body{margin:0;height:100%;background:var(--bg);color:var(--text);font:16px/1.4 system-ui,Segoe UI,Roboto,Arial}");
  h += F(".wrap{max-width:1040px;margin:0 auto;padding:16px 14px 28px} .card{background:var(--panel);border:1px solid #1f2532;border-radius:14px;padding:14px}");
  h += F(".tabs{display:flex;gap:8px;margin:10px 0 14px}.tab{padding:10px 14px;border-radius:12px;border:1px solid #242b3a;background:#1a1f2b;cursor:pointer}.tab.active{outline:2px solid #2b8cff}");
  h += F(".grid{display:grid;gap:12px}.g2{grid-template-columns:repeat(2,1fr)}.g3{grid-template-columns:repeat(3,1fr)}@media(max-width:820px){.g3,.g2{grid-template-columns:1fr}}");
  h += F(".btns{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:10px}@media(max-width:840px){.btns{grid-template-columns:repeat(3,1fr)}}");
  h += F("button{border:1px solid #242b3a;background:#222837;color:#fff;padding:12px 10px;border-radius:12px;font-weight:600;cursor:pointer}button.active{outline:2px solid #2b8cff}");
  h += F(".hidden{display:none}.s{color:#9aa3b2;font-size:13px}.pill{display:inline-block;padding:2px 8px;border-radius:999px;font-size:12px;background:#1f2634;border:1px solid #2a3245;color:#b9c2d3}");
  h += F("input[type=number]{padding:6px;border-radius:8px;border:1px solid #2a3245;background:#0f131c;color:#fff;width:120px}");
  // VFD indicator styles: .vfd-on = green, .vfd-off = yellow, .vfd-miss = red
  h += F(".vfd{display:inline-block;width:12px;height:12px;border-radius:50%;margin-left:8px;border:1px solid #1b1b1b;background:#444;vertical-align:middle}");
  h += F(".vfd-on{background:#21c07a;box-shadow:0 0 6px rgba(33,192,122,0.6)} .vfd-off{background:#ffcf4d;box-shadow:0 0 6px rgba(255,207,77,0.45)} .vfd-miss{background:#ff4d4f;box-shadow:0 0 6px rgba(255,77,79,0.45)}");
  h += F("</style></head><body><div class='wrap'>");

  // Заголовок с версией
  h += F("<h2>Панель <span class='s'>");
  h += FW_VER;
  h += F("</span></h2>");

  // ----- Индикация температур -----
  h += F("<div class='grid g3' style='margin-bottom:12px;'>");
  h += F("<div class='card'><div class='s'>Подача</div><div style='font-size:28px' id='t1'>—</div></div>");
  h += F("<div class='card'><div class='s'>Обратка</div><div style='font-size:28px' id='t2'>—</div></div>");
  h += F("<div class='card'><div class='s'>Дымовые</div><div style='font-size:28px' id='t3'>—</div></div>");
  h += F("</div>");

  // // ----- Кнопки скоростей ПЧ -----
    h += F("<div class='card'><div class='s'>Скорости ПЧ (PC-00..07)</div><div class='btns'><button class='stop' data-speed='0'>СТОП</button>");
  const char* PCL[8] = {"12.5%","25%","37.5%","50%","60%","70%","80%","100%"};
  for (int i=0;i<8;i++){ h += String("<button data-speed='") + (i+1) + "'>" + PCL[i] + "</button>"; }
  h += F("</div></div>");
  // ----- Кнопки скоростей ПЧ -----

  // ----- Блок скоростей ПЧ временно убран -----
  h += F("<!-- Скорости ПЧ (PC-00..07) удалены из UI -->");
  // ----- Режим и состояние -----
  h += F("<div class='grid g2' style='margin-bottom:12px;'><div class='card'>");
  h += F("<div class='s'>Режим</div><select id='boilMode'>");
  h += F("<option value='0'>Выключен</option>");
  h += F("<option value='1'>Ручной</option>");
  h += F("<option value='2'>Розжиг</option>");
  h += F("<option value='3'>Нагрев</option>");
  h += F("<option value='4'>Поддержание</option>");
  h += F("<option value='5'>Фитиль</option>");
  h += F("</select>");
  h += F("<div class='s'>Уставка подачи (&deg;C)</div><input id='setp' type='number' step='1' min='20' max='90'/>");
  h += F("<div style='margin-top:6px'><label><input type='checkbox' id='autoMode'> Автоматический режим</label>");
  h += F("<span style='margin-left:12px' class='s'>VFD:</span>");
  h += F("<span id='vfd1' class='vfd' title='Первичный воздух'></span>");
  h += F("<span id='vfd2' class='vfd' title='Вторичный воздух'></span>");
  h += F("<span id='vfd3' class='vfd' title='Шнек'></span></div>");
  h += F("<br><button id='apply'>Применить</button>");
  h += F("<div class='s' style='margin-top:8px;'>Текущий режим: <b id='modeName'>—</b></div></div>");

  h += F("<div class='card'><div class='s' style='margin-bottom:8px;'>Состояние</div>");
  h += F("<div>RUN: <span id='rRun' class='pill'>—</span></div>");
  h += F("<div>MS1: <span id='rMs1' class='pill'>—</span></div>");
  h += F("<div>MS2: <span id='rMs2' class='pill'>—</span></div>");
  h += F("<div>MS3: <span id='rMs3' class='pill'>—</span></div>");
  h += F("<div>Скорость: <span id='rMode' class='pill'>—</span></div></div></div>");

  // ----- Настройки: вкладки режимов + Общие -----
  h += F("<h2>Настройки</h2><div class='tabs' id='modeTabs'>");
  h += F("<div class='tab active' data-m='1'>Ручной</div>");
  h += F("<div class='tab' data-m='2'>Розжиг</div>");
  h += F("<div class='tab' data-m='3'>Нагрев</div>");
  h += F("<div class='tab' data-m='4'>Поддержание</div>");
  h += F("<div class='tab' data-m='5'>Фитиль</div>");
  h += F("<div class='tab' data-m='0'>Общие</div>");
  h += F("</div>");

  h += F("<div class='card' id='tabContent'>");

  // ====== Контент режимов (унифицированный блок для всех режимов 1..5) ======
  h += F("<div id='viewProfiles'>");
  h += F("<div class='blk blk-unified'>");
  h += F("<div class='s'>Дымовые газы, &deg;C</div>");
  h += F("<div>Низ: <input id='m_Tfl_low' type='number' step='0.1' placeholder='150'> ");
  h += F("Верх: <input id='m_Tfl_high' type='number' step='0.1' placeholder='200'></div><br>");
  h += F("<div class='s'>Параметры розжига</div>");
  h += F("<div>Первичный воздух, % <input id='m_air1' type='number' step='1' placeholder='-1'> ");
  h += F("Вторичный воздух, % <input id='m_air2' type='number' step='1'></div>");
  h += F("<div>Время работы шнека, с <input id='m_ton' type='number' step='0.1'> ");
  h += F("Пауза шнека, с <input id='m_toff' type='number' step='0.1'> ");
  h += F("Подача топлива, кг/час <input id='m_fuel' type='number' step='0.1' readonly tabindex='-1'></div>");
  h += F("</div>");

  // Кнопки работы с профилем
  h += F("<div style='margin-top:10px; display:flex; gap:8px;'>");
  h += F("<button id='cfgLoad'>Загрузить</button><button id='cfgSave'>Сохранить</button>");
  h += F("<button id='cfgApply'>Применить к режиму</button><button id='cfgFactory'>Заводские</button>");
  h += F("</div>");
  h += F("</div>"); // viewProfiles

  // ====== Общие (вкладки) ======
  h += F("<div id='viewCommon' class='hidden'>");
  h += F("<div class='tabs' id='commonTabs'>");
  h += F("<div class='tab active' data-c='air1'>Первичный воздух</div>");
  h += F("<div class='tab' data-c='air2'>Вторичный воздух</div>");
  h += F("<div class='tab' data-c='auger'>Шнек</div>");
  h += F("<div class='tab' data-c='temp'>Температура</div>");
  h += F("<div class='tab' data-c='coef'>Коэффициенты</div><div class='tab' data-c='alarm'>Аварийный режим</div>");
  h += F("</div>");

  // Первичный воздух — вентилятор 1 (10 ступеней, %)
  h += F("<div id='cAir1'>");
  h += F("<div class='s'>Вентилятор 1 — 10 ступеней, %</div>");
  h += F("<div id='fan110' style='display:flex;flex-wrap:wrap;gap:6px'></div>");
  h += F("</div>");

  // Вторичный воздух — вентилятор 2 (10 ступеней, %)
  h += F("<div id='cAir2' class='hidden'>");
  h += F("<div class='s'>Вентилятор 2 — 10 ступеней, %</div>");
  h += F("<div id='fan210' style='display:flex;flex-wrap:wrap;gap:6px'></div>");
  h += F("</div>");

  // Шнек — общие
  h += F("<div id='cAuger' class='hidden'>");
  h += F("<div class='s'>Шнек - 10 ступеней, %</div>");
  h += F("<div id='aug10' style='display:flex;flex-wrap:wrap;gap:6px; margin-bottom:8px'></div>");
  h += F("<div class='grid g2'>");
  h += F("<div class='s'>Мин. время работы шнека (мс)</div><input id='g_minTon' type='number' step='10'>");
  h += F("<div class='s'>Мин. пауза шнека (мс)</div><input id='g_minToff' type='number' step='10'>");
  h += F("<div class='s'>Q100 (кг/ч @100%)</div><input id='g_q100' type='number' step='0.001'>");
  h += F("<div class='s'>K материала</div><input id='g_kmat' type='number' step='0.001'>");
  h += F("</div>");
  h += F("</div>");

  // Температура — Tmax и верх дымовых
  h += F("<div id='cTemp' class='hidden'>");
  h += F("<div class='s'>Макс. T подачи (&deg;C)</div><input id='g_Tmax' type='number' step='0.1'>");
  h += F("<div class='s' style='margin-top:8px'>Окно дымовых, &deg;C: верх</div><input id='g_TflueHigh' type='number' step='0.1'>");
  h += F("</div>");

  // Коэффициенты — P/I
  h += F("<div id='cCoef' class='hidden'>");
  h += F("<div class='s'>Коэффициенты регулятора</div>");
  h += F("<div>P <input id='g_Kp' type='number' step='0.001'> I <input id='g_Ki' type='number' step='0.0001'></div>");
  h += F("</div>");

  
  // Аварийный режим — UI
  h += F("<div id='cAlarm' class='hidden'>");

  // Температуры
  h += F("<div class='s' style='margin-top:6px'>Температуры</div>");
  h += F("<div class='grid g2'>"
         "<div class='s'>Макс. T подачи (&deg;C)</div><input id='al_TfeedMax' type='number' step='0.1'>"
         "<div class='s'>Макс. T обратки (&deg;C)</div><input id='al_TreturnMax' type='number' step='0.1'>"
         "<div class='s'>Дымовые — верхний аварийный порог (&deg;C)</div><input id='al_TflueMax' type='number' step='0.1'>"
         "<div class='s'>«Нет пламени» по дымовым — порог (&deg;C)</div><input id='al_noFlameThresh' type='number' step='0.1'>"
         "<div class='s'>«Нет пламени» — выдержка, с (Розжиг)</div><input id='al_noFlameHoldIgn' type='number' step='1'>"
         "<div class='s'>«Нет пламени» — выдержка, с (Разогрев)</div><input id='al_noFlameHoldHeat' type='number' step='1'>"
         "<div class='s'>«Нет пламени» — выдержка, с (Поддержание)</div><input id='al_noFlameHoldKeep' type='number' step='1'>"
         "</div>");

  // Динамика
  h += F("<div class='s' style='margin-top:10px'>Динамика</div>");
  h += F("<div class='grid g2'>"
         "<div class='s'>Макс. скорость роста T подачи (&deg;C/мин)</div><input id='al_TfeedRateMax' type='number' step='0.1'>"
         "<div class='s'>&#916;T = подача − обратка — максимум (&deg;C)</div><input id='al_deltaTmax' type='number' step='0.1'>"
         "<div class='s'>&#916;T — выдержка превышения, с</div><input id='al_deltaTHold' type='number' step='1'>"
         "</div>");

  // Приводы / ПЧ
  h += F("<div class='s' style='margin-top:10px'>Приводы / ПЧ</div>");
  h += F("<div class='grid g2'>"
         "<label><input id='al_sn_fault' type='checkbox'> Шнек — FAULT ⇒ авария</label><div></div>"
         "<div class='s'>Шнек — RUN не появился за, с</div><input id='al_sn_run_to' type='number' step='1'>"
         "<label><input id='al_v1_fault' type='checkbox'> Вентилятор 1 — FAULT ⇒ авария</label><div></div>"
         "<div class='s'>Вентилятор 1 — RUN не появился за, с</div><input id='al_v1_run_to' type='number' step='1'>"
         "<label><input id='al_v2_fault' type='checkbox'> Вентилятор 2 — FAULT ⇒ авария</label><div></div>"
         "<div class='s'>Вентилятор 2 — RUN не появился за, с</div><input id='al_v2_run_to' type='number' step='1'>"
         "</div>");

  // Таймауты стадий
  h += F("<div class='s' style='margin-top:10px'>Таймауты стадий</div>");
  h += F("<div class='grid g2'>"
         "<div class='s'>Розжиг — таймаут, с</div><input id='al_ign_timeout' type='number' step='1'>"
         "<div class='s'>Разогрев — таймаут, с</div><input id='al_warmup_timeout' type='number' step='1'>"
         "</div>");

  // Датчики (санити-чек)
  h += F("<div class='s' style='margin-top:10px'>Датчики</div>");
  h += F("<div class='grid g2'>"
         "<div class='s'>Тайм-аут получения температуры, с</div><input id='al_temp_timeout' type='number' step='1' value='20'>"
         "<div class='s'>Допустимый диапазон температуры: мин (&deg;C)</div><input id='al_temp_min' type='number' step='0.1' value='0'>"
         "<div class='s'>Допустимый диапазон температуры: макс (&deg;C)</div><input id='al_temp_max' type='number' step='0.1' value='100'>"
         "<label><input id='al_sens_feed' type='checkbox' checked> Контролировать — Подача</label><div></div>"
         "<label><input id='al_sens_return' type='checkbox' checked> Контролировать — Обратка</label><div></div>"
         "<label><input id='al_sens_flue' type='checkbox' checked> Контролировать — Дымовые</label><div></div>"
         "</div>");

  h += F("</div>");
// Кнопки общих
  h += F("<div style='margin-top:10px'><button id='gLoad'>Загрузить</button> <button id='gSave'>Сохранить</button></div>");

  h += F("</div>"); // viewCommon

  h += F("</div>"); // tabContent

  // ----- Журнал -----
  h += F("<h2>Журнал</h2><div class='card'><pre id='logs'>—</pre><button id='btnReloadLogs'>Обновить</button></div>");

  // ====== SCRIPT ======
  h += F("<script>const $=s=>document.querySelector(s), $$=s=>Array.from(document.querySelectorAll(s));");
  h += F("function pill(el,v){el.textContent=v?'ВКЛ':'ВЫКЛ'; el.style.background=v?'#0d2a1f':'#2a2333';}");
  h += F("(function(){const grid=document.querySelector('.grid.g2');if(!grid)return;const cards=grid.querySelectorAll('.card');if(cards.length<2)return;if(document.getElementById('feedKgph'))return;const row=document.createElement('div');row.innerHTML='Подача топлива: <span id=\"feedKgph\">-</span> <span class=\"s\">кг/ч</span>';cards[1].appendChild(row);}());");
  // VFD indicator helper
  h += F("function setVFD(id,state){const el=document.getElementById(id); if(!el) return; el.className='vfd'; if(state==='missing') el.classList.add('vfd-miss'); else if(state==='on') el.classList.add('vfd-on'); else el.classList.add('vfd-off'); }");
  // ----- Управление кнопками скоростей ПЧ -----
  h += F("function setSpeedButtons(a){ $$('.btns button').forEach(b=>b.classList.toggle('active', Number(b.dataset.speed)===a)); }");
  h += F("async function sendSpeed(s){ try{ const r=await fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json; charset=utf-8'},body:JSON.stringify({speed:Number(s)})}); if(!r.ok) throw new Error('HTTP '+r.status); const j=await r.json(); setSpeedButtons(Number(j.speed||0)); }catch(e){ alert('Ошибка отправки скорости: '+(e.message||e)); } }");
  h += F("$$('.btns button').forEach(b=>b.addEventListener('click',()=>sendSpeed(b.dataset.speed)));");
   h += F("async function fetchStatus(){try{const r=await fetch('/api/status',{cache:'no-store'}); const j=await r.json();");
   h += F("$('#t1').textContent=(j.temp&&j.temp.ds1!=null)?Number(j.temp.ds1).toFixed(1):'—';");
   h += F("$('#t2').textContent=(j.temp&&j.temp.ds2!=null)?Number(j.temp.ds2).toFixed(1):'—';");
   h += F("$('#t3').textContent=(j.temp&&j.temp.stack!=null)?Number(j.temp.stack).toFixed(1):'—';");
   h += F("pill($('#rRun'),j.relays&&j.relays.run==='true'); pill($('#rMs1'),j.relays&&j.relays.ms1==='true'); pill($('#rMs2'),j.relays&&j.relays.ms2==='true'); pill($('#rMs3'),j.relays&&j.relays.ms3==='true');");
   h += F("const sp=Number(j.speed||0); $('#rMode').textContent=isFinite(sp)?sp:'—';");
  h += F("const feedVal=(j.feed_kgph!==undefined&&j.feed_kgph!==null)?Number(j.feed_kgph):NaN; const feedEl=id('feedKgph'); if(feedEl){ feedEl.textContent=isFinite(feedVal)?feedVal.toFixed(1):'-'; }");
   h += F("const vv=j.vfd||{}; setVFD('vfd1', (vv.v1_conn==='true') ? (vv.v1_on==='true'?'on':'off') : 'missing'); setVFD('vfd2', (vv.v2_conn==='true') ? (vv.v2_on==='true'?'on':'off') : 'missing'); setVFD('vfd3', (vv.v3_conn==='true') ? (vv.v3_on==='true'?'on':'off') : 'missing');");
   h += F("$('#modeName').textContent=(j.boiler&&j.boiler.modeName)?j.boiler.modeName:'—'; $('#setp').value=(j.boiler&&j.boiler.setpoint)?j.boiler.setpoint:60;");
   h += F("}catch(e){}}");
  h += F("async function applyMode(){try{const mode=Number(document.getElementById('boilMode').value||0);const setp=Number(document.getElementById('setp').value||0);await fetch('/api/boiler',{method:'POST',headers:{'Content-Type':'application/json; charset=utf-8'},body:JSON.stringify({mode:mode,setpoint:setp})});await fetchStatus();alert('Применено');}catch(e){alert('Ошибка: '+e.message);} }");
  h += F("$('#apply').addEventListener('click',applyMode);");
  h += F("const amEl=document.getElementById('autoMode'); if(amEl){ const v=localStorage.getItem('autoMode')==='1'; amEl.checked=v; amEl.addEventListener('change',()=>localStorage.setItem('autoMode', amEl.checked?'1':'0')); }");

  // ——— Вкладки режимов ———
  h += F("let cfgMode=1; function showCommonView(on){ $('#viewProfiles').classList.toggle('hidden', on); $('#viewCommon').classList.toggle('hidden', !on);} ");
  h += F("function applyModeVisibility(){ const m=cfgMode; const on=(m>=1&&m<=5); document.querySelector('.blk-unified').style.display=on?'block':'none'; }");
  h += F("function tabSel(m){ cfgMode=m; $$('#modeTabs .tab').forEach(t=>t.classList.toggle('active', Number(t.dataset.m)===m)); if(m===0){ showCommonView(true); gLoad(); } else { showCommonView(false); applyModeVisibility(); cfgLoad(); } }");
  h += F("$$('#modeTabs .tab').forEach(t=>t.addEventListener('click',()=>tabSel(Number(t.dataset.m))));");

  // ——— Общие: подвкладки ———
  h += F("let cMode='air1'; function id(x){return document.getElementById(x);}");
  h += F("function commonShow(k){ cMode=k; const map={air1:'cAir1',air2:'cAir2',auger:'cAuger',temp:'cTemp',coef:'cCoef',alarm:'cAlarm'}; for(const key in map){ id(map[key]).classList.toggle('hidden', key!==k);} $$('#commonTabs .tab').forEach(t=>t.classList.toggle('active', t.dataset.c===k)); }");
  h += F("$$('#commonTabs .tab').forEach(t=>t.addEventListener('click',()=>commonShow(t.dataset.c)));");

  // ——— CRUD профилей ———
  h += F("async function cfgLoad(){ const r=await fetch('/api/profile?m='+cfgMode); const c=await r.json();");
  h += F("id('m_Tfl_low').value=(c.Tflue_low!=null?c.Tflue_low:150); id('m_Tfl_high').value=(c.Tflue_high!=null?c.Tflue_high:200);");
  h += F("}");
  h += F("async function cfgSave(){ const body={ Tflue_low:Number(id('m_Tfl_low').value||0), Tflue_high:Number(id('m_Tfl_high').value||0) }; await fetch('/api/profile?m='+cfgMode,{method:'POST',headers:{'Content-Type':'application/json; charset=utf-8'},body:JSON.stringify(body)}); alert('Сохранено'); }");
  h += F("async function cfgApply(){ await fetch('/api/boiler',{method:'POST',headers:{'Content-Type':'application/json; charset=utf-8'},body:JSON.stringify({mode:cfgMode})}); await fetchStatus(); alert('Применено'); }");
  h += F("async function cfgFactory(){ await fetch('/api/profile?m='+cfgMode+'&reset=1'); await cfgLoad(); alert('Сброшено к заводским'); }");
  h += F("$('#cfgLoad').addEventListener('click',cfgLoad); $('#cfgSave').addEventListener('click',cfgSave); $('#cfgApply').addEventListener('click',cfgApply); $('#cfgFactory').addEventListener('click',cfgFactory);");

  // ——— Общие (гриды) ———
  h += F("function build10(id,prefix){ const c=document.getElementById(id); if(!c) return; c.innerHTML=''; for(let i=0;i<10;i++){ const inp=document.createElement('input'); inp.id=prefix+i; inp.type='number'; inp.step='10'; inp.min='10'; inp.max='100'; inp.value=(i+1)*10; inp.style.width='70px'; inp.style.padding='6px'; inp.style.borderRadius='8px'; inp.style.border='1px solid #2a3245'; c.appendChild(inp);} }");
  h += F("build10('fan110','gf1'); build10('fan210','gf2'); build10('aug10','ga');");
  h += F("async function gLoad(){ try{ const r=await fetch('/api/common'); if(!r.ok) throw new Error('HTTP '+r.status); const j=await r.json();");
  h += F("id('g_minTon').value=j.amin_on||0; id('g_minToff').value=j.amin_off||0; id('g_Tmax').value=j.tmax||0; id('g_TflueHigh').value=j.tflue_high||0;");
  h += F("id('g_Kp').value=j.kp||0; id('g_Ki').value=j.ki||0;");
  h += F("id('g_q100').value=(j.q100!==undefined&&j.q100!==null)?j.q100:10;");
  h += F("id('g_kmat').value=(j.kmat!==undefined&&j.kmat!==null)?j.kmat:1;");
  h += F("const fan1 = j.fan || j.fan1 || []; const fan2 = j.fan2 || []; const aug  = j.aug10 || []; for(let i=0;i<10;i++){ var e1=id('gf1'+i); if(e1) e1.value = fan1[i]||0; var e2=id('gf2'+i); if(e2) e2.value = fan2[i]||0; var ea=id('ga'+i); if(ea) ea.value = aug[i]||0; }");
  h += F("}catch(e){ alert('Ошибка загрузки общих: '+e.message); } }");
  h += F("async function gSave(){ try{ const body={ amin_on:Number(id('g_minTon').value||0), amin_off:Number(id('g_minToff').value||0), tmax:Number(id('g_Tmax').value||0), tflue_high:Number(id('g_TflueHigh').value||0), kp:Number(id('g_Kp').value||0), ki:Number(id('g_Ki').value||0), q100:Number(id('g_q100').value||10), kmat:Number(id('g_kmat').value||1), fan:Array.from({length:10},(_,i)=>Number((id('gf1'+i)&&id('gf1'+i).value)||0)), fan2:Array.from({length:10},(_,i)=>Number((id('gf2'+i)&&id('gf2'+i).value)||0)), aug10:Array.from({length:10},(_,i)=>Number((id('ga'+i)&&id('ga'+i).value)||0)) };");
  h += F("await fetch('/api/common',{method:'POST',headers:{'Content-Type':'application/json; charset=utf-8'},body:JSON.stringify(body)}); alert('Общие сохранены'); }catch(e){ alert('Ошибка сохранения: '+e.message);} }");
  h += F("id('gLoad').addEventListener('click', gLoad); id('gSave').addEventListener('click', gSave);");

  // ——— Запуск ———
  h += F("fetchStatus(); setInterval(fetchStatus,1000); cfgLoad(); gLoad(); applyModeVisibility();");
  h += F("</script></div></body></html>");
  return h;
}

// ========== Роутинг ==========
static void handleRoot(){ server.sendHeader("Cache-Control","no-store"); server.send(200,"text/html; charset=utf-8", INDEX_HTML()); }

void webInit(){
  // Убедиться, что реле RUN настроено как выход и в состоянии, соответствующем g_speed_mode
  pinMode(PIN_RELAY_RUN, OUTPUT);
  // Используем setRelay чтобы учитывать RELAY_ACTIVE_LOW
  setRelay(PIN_RELAY_RUN, (g_speed_mode > 0));

   server.on("/", HTTP_GET, handleRoot);
   server.on("/api/status", HTTP_GET, handleStatus);
   server.on("/api/logs", HTTP_GET, handleLogs);
   server.on("/api/cmd", HTTP_POST, handleCmd);
   server.on("/api/boiler", HTTP_POST, handleBoiler);
   server.on("/api/profile", HTTP_GET, handleProfileGet);
   server.on("/api/profile", HTTP_POST, handleProfilePost);
   server.on("/api/common", HTTP_GET, handleCommonGet);   // <-- добавлено
   server.on("/api/common", HTTP_POST, handleCommonPost); // <-- добавлено
   // /api/common — реализуется в соответствующем модуле (UI использует tmax, tflue_high, kp, ki, fan[], fan2[], aug10[], amin_on/off)
   server.onNotFound([](){ server.send(404,"text/plain; charset=utf-8","Not found"); });
   server.begin();
 }

 void webTick(){
  server.handleClient();
  static int lastOn = -1;
  int wantedOn = (g_speed_mode > 0) ? 1 : 0;
  if (lastOn != wantedOn){
    setRelay(PIN_RELAY_RUN, wantedOn);
    lastOn = wantedOn;
    Serial.println(String("[HW] webTick: enforced RUN = ") + (wantedOn? "ON":"OFF"));
  }
}
