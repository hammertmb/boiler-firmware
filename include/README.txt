BoilerControl_ESP32 v10 (full)
==============================
• Разбивка по файлам: pins / logging / profiles / sensors / vfd / control / web / ui_index.
• Веб‑панель: вкладки «Панель» и «Настройки», 8 пресетов скоростей (PC‑00..07), API для профилей.
• Сохранение профилей — в NVS (Preferences), бинарно для надёжности.

Компиляция
----------
Arduino IDE 2.x, плата ESP32. Требуемые библиотеки:
- DallasTemperature
- OneWire
- max6675

Провода ПЧ (SKI780)
-------------------
DI1=RUN ← GPIO23
DI2=MR3 ← GPIO22
DI4=MR2 ← GPIO19
DI5=MR1 ← GPIO21
Полярность входов у вас низкоактивная (замыкание на GND) — оставлен флаг RELAY_ACTIVE_LOW=1.

IP: при режиме точки доступа — 192.168.4.1
