// ============================================================================
//  gw_params.hpp — постоянные параметры прошивки (NVS) и Serial-консоль.
//
//  Все настройки шлюза хранятся в энергонезависимой памяти ESP32 (NVS,
//  библиотека Preferences) и переживают отключение питания. Реализация:
//    * таблица параметров: имя + адрес переменной + тип + размер. Добавление
//      нового параметра — одна строка в таблице и одна ветка в setParam();
//    * значения загружаются при старте и пишутся обратно только изменённые,
//      не чаще одного раза в секунду (flash NVS не любит частые записи);
//    * при несовпадении версии структуры сохранённые значения сбрасываются
//      к значениям по умолчанию;
//    * после загрузки значения проверяются на допустимость (порты различны,
//      таймауты в разумных пределах и т.п.).
//
//  Консоль (Serial Monitor, 115200, команда + Enter):
//    help | show | set <ключ> <значение> | save | reset | reboot | wifi
//
//  Ключи: wifi_ssid, wifi_pass, wifi_type, mac, ip, gateway, subnet, dns,
//         port_sim1, port_sim2, port_out1, port_out2, log_level,
//         modem1_type, modem2_type, idle_timeout,
//         director_en, director_port, pool_port, director_ip, director_auto,
//         box_name
//
//  Файл специфичен для Arduino/ESP32 и используется только прошивкой; ядро
//  gw_core.hpp и координация gw_director.hpp от него не зависят.
// ============================================================================

#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

namespace gp {

enum LogLevel : uint8_t {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3
};

// Профиль Wi-Fi. COMPAT — совместимость с капризными маршрутизаторами:
// 802.11 b/g/n без энергосбережения (наиболее надёжный вариант для CSD).
enum WifiType : uint8_t {
    WIFI_AUTO     = 0,   // универсально: sleep off, PHY не ограничиваем
    WIFI_COMPAT   = 1,   // b/g/n, sleep off
    WIFI_STANDARD = 2    // настройки драйвера по умолчанию
};

// ------------------------------ параметры ----------------------------------

struct Params {
    char     wifiSsid[33];
    char     wifiPass[65];
    uint8_t  wifiType;
    char     mac[18];          // пусто = аппаратный MAC
    char     ip[16];           // пусто или 0.0.0.0 = DHCP
    char     gateway[16];
    char     subnet[16];
    char     dns[16];          // пусто = DNS из DHCP
    uint16_t portSim1;         // TCP-порт линии SIM1
    uint16_t portSim2;         // TCP-порт линии SIM2
    uint16_t portOut1;         // справочный внешний (NAT) порт SIM1
    uint16_t portOut2;         // справочный внешний (NAT) порт SIM2
    uint8_t  logLevel;
    uint8_t  modem1Type;       // профиль модема SIM1 (gw::ModemProfile)
    uint8_t  modem2Type;       // профиль модема SIM2
    uint32_t idleTimeoutMs;    // простой вызова до разрыва (180 с)
    uint8_t  directorEn;       // 1 = эта коробка работает дирижёром парка
    uint16_t directorPort;     // порт агентских соединений + UDP-объявлений
    uint16_t poolPort;         // порт пула для клиентов дирижёра
    char     directorIp[16];   // адрес дирижёра для агента; пусто = автопоиск
    uint8_t  directorAuto;     // автопоиск дирижёра по UDP-объявлению
    char     boxName[16];      // имя коробки в парке; пусто = gw-<хвост MAC>
};

static const char     NVS_NAMESPACE[] = "gwcfg";
static const uint16_t CONFIG_VERSION  = 2;
static const size_t   PARAM_CACHE_MAX = 68;   // хватает самому длинному параметру

enum ParamType : uint8_t { P_STRING, P_U8, P_U16, P_U32 };

struct Param {
    const char* name;
    void*       value;
    ParamType   type;
    size_t      size;
    uint8_t     cache[PARAM_CACHE_MAX];   // последняя записанная копия
};

static Params      params;
static Preferences storage;
static bool        storageReady = false;

static Param kParams[] = {
    {"wifi_ssid",    params.wifiSsid,       P_STRING, sizeof(params.wifiSsid),       {0}},
    {"wifi_pass",    params.wifiPass,       P_STRING, sizeof(params.wifiPass),       {0}},
    {"wifi_type",    &params.wifiType,      P_U8,     sizeof(params.wifiType),       {0}},
    {"mac",          params.mac,            P_STRING, sizeof(params.mac),            {0}},
    {"ip",           params.ip,             P_STRING, sizeof(params.ip),             {0}},
    {"gateway",      params.gateway,        P_STRING, sizeof(params.gateway),        {0}},
    {"subnet",       params.subnet,         P_STRING, sizeof(params.subnet),         {0}},
    {"dns",          params.dns,            P_STRING, sizeof(params.dns),            {0}},
    {"port_sim1",    &params.portSim1,      P_U16,    sizeof(params.portSim1),       {0}},
    {"port_sim2",    &params.portSim2,      P_U16,    sizeof(params.portSim2),       {0}},
    {"port_out1",    &params.portOut1,      P_U16,    sizeof(params.portOut1),       {0}},
    {"port_out2",    &params.portOut2,      P_U16,    sizeof(params.portOut2),       {0}},
    {"log_level",    &params.logLevel,      P_U8,     sizeof(params.logLevel),       {0}},
    {"modem1_type",  &params.modem1Type,    P_U8,     sizeof(params.modem1Type),     {0}},
    {"modem2_type",  &params.modem2Type,    P_U8,     sizeof(params.modem2Type),     {0}},
    {"idle_timeout", &params.idleTimeoutMs, P_U32,    sizeof(params.idleTimeoutMs),  {0}},
    {"director_en",  &params.directorEn,    P_U8,     sizeof(params.directorEn),     {0}},
    {"director_port",&params.directorPort,  P_U16,    sizeof(params.directorPort),   {0}},
    {"pool_port",    &params.poolPort,      P_U16,    sizeof(params.poolPort),       {0}},
    {"director_ip",  params.directorIp,     P_STRING, sizeof(params.directorIp),     {0}},
    {"director_auto",&params.directorAuto,  P_U8,     sizeof(params.directorAuto),   {0}},
    {"box_name",     params.boxName,        P_STRING, sizeof(params.boxName),        {0}},
};

static const size_t PARAM_COUNT = sizeof(kParams) / sizeof(kParams[0]);

static void setDefaults() {
    memset(&params, 0, sizeof(params));
    params.wifiType      = WIFI_COMPAT;
    params.portSim1      = 5000;
    params.portSim2      = 5001;
    params.logLevel      = LOG_INFO;
    params.idleTimeoutMs = 180000;
    params.directorPort  = 5010;
    params.poolPort      = 5020;
    params.directorAuto  = 1;
}

// -------------------------- загрузка и сохранение --------------------------

static bool writeToNvs(Param& p) {
    switch (p.type) {
        case P_STRING:
            return storage.putString(p.name, static_cast<const char*>(p.value)) > 0;
        case P_U8:
            return storage.putUChar(p.name, *static_cast<uint8_t*>(p.value)) > 0;
        case P_U16:
            return storage.putUShort(p.name, *static_cast<uint16_t*>(p.value)) > 0;
        case P_U32:
            return storage.putULong(p.name, *static_cast<uint32_t*>(p.value)) > 0;
    }
    return false;
}

static bool readFromNvs(Param& p) {
    if (!storage.isKey(p.name)) return false;
    switch (p.type) {
        case P_STRING: {
            String value = storage.getString(p.name, "");
            if (value.length() >= p.size) return false;
            memset(p.value, 0, p.size);
            memcpy(p.value, value.c_str(), value.length());
            return true;
        }
        case P_U8:
            *static_cast<uint8_t*>(p.value) =
                storage.getUChar(p.name, *static_cast<uint8_t*>(p.value));
            return true;
        case P_U16:
            *static_cast<uint16_t*>(p.value) =
                storage.getUShort(p.name, *static_cast<uint16_t*>(p.value));
            return true;
        case P_U32:
            *static_cast<uint32_t*>(p.value) =
                storage.getULong(p.name, *static_cast<uint32_t*>(p.value));
            return true;
    }
    return false;
}

// Проверка значений после загрузки из flash: повреждённые или конфликтные
// значения заменяются безопасными.
static void validateParams() {
    if (params.portSim1 == 0) params.portSim1 = 5000;
    if (params.portSim2 == 0) params.portSim2 = 5001;
    if (params.portSim1 == params.portSim2) {
        params.portSim2 = (params.portSim1 == 5001) ? 5000 : 5001;
    }
    if (params.directorPort == 0) params.directorPort = 5010;
    if (params.poolPort == 0)     params.poolPort = 5020;
    if (params.directorPort == params.portSim1 || params.directorPort == params.portSim2 ||
        params.directorPort == params.poolPort) {
        params.directorPort = 5010;
    }
    if (params.poolPort == params.portSim1 || params.poolPort == params.portSim2 ||
        params.poolPort == params.directorPort) {
        params.poolPort = 5020;
    }
    if (params.logLevel > LOG_DEBUG)  params.logLevel = LOG_INFO;
    if (params.wifiType > WIFI_STANDARD) params.wifiType = WIFI_COMPAT;
    if (params.modem1Type > 3) params.modem1Type = 0;
    if (params.modem2Type > 3) params.modem2Type = 0;
    if (params.idleTimeoutMs < 10000 || params.idleTimeoutMs > 3600000UL) {
        params.idleTimeoutMs = 180000;
    }
}

static void loadParams() {
    setDefaults();
    storageReady = storage.begin(NVS_NAMESPACE, false);
    if (!storageReady) {
        Serial.println("[PARAM] NVS недоступен — работаем со значениями по умолчанию");
        return;
    }
    const uint16_t storedVersion = storage.getUShort("version", 0);
    if (storedVersion != CONFIG_VERSION) {
        storage.clear();
        storage.putUShort("version", CONFIG_VERSION);
    }
    for (size_t i = 0; i < PARAM_COUNT; ++i) {
        readFromNvs(kParams[i]);
        memcpy(kParams[i].cache, kParams[i].value, kParams[i].size);
    }
    validateParams();
    Serial.printf("[PARAM] Загружено параметров: %u (namespace '%s')\n",
                  (unsigned)PARAM_COUNT, NVS_NAMESPACE);
}

// Записываются только изменившиеся параметры и не чаще раза в секунду.
static void syncParams(bool force = false) {
    static uint32_t lastSyncAt = 0;
    if (!storageReady) return;
    if (!force && (uint32_t)(millis() - lastSyncAt) < 1000) return;
    lastSyncAt = millis();

    bool anyChanged = false;
    for (size_t i = 0; i < PARAM_COUNT; ++i) {
        Param& p = kParams[i];
        if (!force && memcmp(p.value, p.cache, p.size) == 0) continue;
        if (writeToNvs(p)) {
            memcpy(p.cache, p.value, p.size);
            anyChanged = true;
        } else {
            Serial.printf("[PARAM] ошибка записи параметра '%s' в NVS\n", p.name);
        }
    }
    if (anyChanged) Serial.println("[PARAM] изменённые параметры сохранены (NVS)");
}

// ------------------------------ значения -----------------------------------

static bool parseU(const char* text, uint32_t minValue, uint32_t maxValue, uint32_t* out) {
    if (!text || !text[0] || !out) return false;
    char* end = nullptr;
    unsigned long value = strtoul(text, &end, 10);
    if (*end != '\0' || value < minValue || value > maxValue) return false;
    *out = (uint32_t)value;
    return true;
}

static bool validIPv4(const char* s) {
    IPAddress parsed;
    return parsed.fromString(s);
}

static bool parseMacAddress(const char* text, uint8_t mac[6]) {
    if (!text || strlen(text) != 17) return false;
    unsigned int b[6];
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    bool allZero = true, allOnes = true;
    for (int i = 0; i < 6; ++i) {
        if (b[i] > 255) return false;
        mac[i] = (uint8_t)b[i];
        if (b[i] != 0x00) allZero = false;
        if (b[i] != 0xFF) allOnes = false;
    }
    return !allZero && !allOnes && (mac[0] & 0x01) == 0;   // только unicast
}

static bool copyStr(char* dst, size_t cap, const char* src) {
    if (!dst || !src || strlen(src) >= cap) return false;
    memset(dst, 0, cap);
    memcpy(dst, src, strlen(src));
    return true;
}

static void trimUnquote(char* value) {
    if (!value) return;
    while (*value && isspace((unsigned char)*value)) ++value;
    size_t len = strlen(value);
    while (len && isspace((unsigned char)value[len - 1])) value[--len] = '\0';
    if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
    }
}

// Установка одного параметра по имени; возвращает false при неверном ключе
// или значении. Значения применяются сразу (в RAM) и попадают в NVS в syncParams.
static bool setParam(const char* key, const char* value) {
    if (!key || !value) return false;
    uint32_t num = 0;

    if (!strcasecmp(key, "wifi_ssid")) return copyStr(params.wifiSsid, sizeof(params.wifiSsid), value);
    if (!strcasecmp(key, "wifi_pass")) return copyStr(params.wifiPass, sizeof(params.wifiPass), value);
    if (!strcasecmp(key, "wifi_type")) {
        if      (!strcasecmp(value, "auto"))     { params.wifiType = WIFI_AUTO; return true; }
        else if (!strcasecmp(value, "compat"))   { params.wifiType = WIFI_COMPAT; return true; }
        else if (!strcasecmp(value, "standard")) { params.wifiType = WIFI_STANDARD; return true; }
        else return parseU(value, 0, WIFI_STANDARD, &num) && (params.wifiType = (uint8_t)num, true);
    }
    if (!strcasecmp(key, "mac")) {
        if (!strcasecmp(value, "auto")) { params.mac[0] = '\0'; return true; }
        uint8_t m[6];
        return parseMacAddress(value, m) && copyStr(params.mac, sizeof(params.mac), value);
    }
    if (!strcasecmp(key, "ip")) {
        if (!strcasecmp(value, "dhcp")) { params.ip[0] = '\0'; return true; }
        return validIPv4(value) && copyStr(params.ip, sizeof(params.ip), value);
    }
    if (!strcasecmp(key, "gateway")) return validIPv4(value) && copyStr(params.gateway, sizeof(params.gateway), value);
    if (!strcasecmp(key, "subnet"))  return validIPv4(value) && copyStr(params.subnet, sizeof(params.subnet), value);
    if (!strcasecmp(key, "dns")) {
        if (!strcasecmp(value, "none")) { params.dns[0] = '\0'; return true; }
        return validIPv4(value) && copyStr(params.dns, sizeof(params.dns), value);
    }
    if (!strcasecmp(key, "port_sim1") || !strcasecmp(key, "port_sim2")) {
        if (!parseU(value, 1, 65535, &num)) return false;
        uint16_t* target = strcasecmp(key, "port_sim1") == 0 ? &params.portSim1 : &params.portSim2;
        uint16_t  other  = strcasecmp(key, "port_sim1") == 0 ? params.portSim2 : params.portSim1;
        if ((uint16_t)num == other) return false;
        *target = (uint16_t)num;
        return true;
    }
    if (!strcasecmp(key, "port_out1")) return parseU(value, 0, 65535, &num) && (params.portOut1 = (uint16_t)num, true);
    if (!strcasecmp(key, "port_out2")) return parseU(value, 0, 65535, &num) && (params.portOut2 = (uint16_t)num, true);
    if (!strcasecmp(key, "log_level")) {
        if      (!strcasecmp(value, "error")) { params.logLevel = LOG_ERROR; return true; }
        else if (!strcasecmp(value, "warn") || !strcasecmp(value, "warning")) { params.logLevel = LOG_WARN; return true; }
        else if (!strcasecmp(value, "info"))  { params.logLevel = LOG_INFO; return true; }
        else if (!strcasecmp(value, "debug")) { params.logLevel = LOG_DEBUG; return true; }
        else return parseU(value, 0, LOG_DEBUG, &num) && (params.logLevel = (uint8_t)num, true);
    }
    if (!strcasecmp(key, "modem1_type") || !strcasecmp(key, "modem2_type")) {
        uint8_t t;
        if      (!strcasecmp(value, "standard"))       t = 0;
        else if (!strcasecmp(value, "no_ok_on_ath"))   t = 1;
        else if (!strcasecmp(value, "slow"))           t = 2;
        else if (!strcasecmp(value, "no_ok_on_escape")) t = 3;
        else return false;
        if (!strcasecmp(key, "modem1_type")) params.modem1Type = t;
        else                                 params.modem2Type = t;
        return true;
    }
    if (!strcasecmp(key, "idle_timeout")) return parseU(value, 10000, 3600000UL, &num) && (params.idleTimeoutMs = num, true);
    if (!strcasecmp(key, "director_en")) {
        if      (!strcasecmp(value, "on")  || !strcmp(value, "1")) { params.directorEn = 1; return true; }
        else if (!strcasecmp(value, "off") || !strcmp(value, "0")) { params.directorEn = 0; return true; }
        return false;
    }
    if (!strcasecmp(key, "director_port")) {
        if (!parseU(value, 1, 65535, &num)) return false;
        if ((uint16_t)num == params.portSim1 || (uint16_t)num == params.portSim2 ||
            (uint16_t)num == params.poolPort) return false;
        params.directorPort = (uint16_t)num;
        return true;
    }
    if (!strcasecmp(key, "pool_port")) {
        if (!parseU(value, 1, 65535, &num)) return false;
        if ((uint16_t)num == params.portSim1 || (uint16_t)num == params.portSim2 ||
            (uint16_t)num == params.directorPort) return false;
        params.poolPort = (uint16_t)num;
        return true;
    }
    if (!strcasecmp(key, "director_ip")) {
        if (!strcasecmp(value, "none")) { params.directorIp[0] = '\0'; return true; }
        return validIPv4(value) && copyStr(params.directorIp, sizeof(params.directorIp), value);
    }
    if (!strcasecmp(key, "director_auto")) {
        if      (!strcasecmp(value, "on")  || !strcmp(value, "1")) { params.directorAuto = 1; return true; }
        else if (!strcasecmp(value, "off") || !strcmp(value, "0")) { params.directorAuto = 0; return true; }
        return false;
    }
    if (!strcasecmp(key, "box_name")) return copyStr(params.boxName, sizeof(params.boxName), value);

    return false;
}

// ------------------------------- консоль -----------------------------------

class Console {
public:
    void process() {                          // вызывать из loop() — не блокирует
        while (Serial.available() > 0) {
            int v = Serial.read();
            if (v < 0) break;
            char c = (char)v;
            if (c == '\r') continue;
            if (c == '\n') {
                line_[used_] = '\0';
                handleLine(line_);
                used_ = 0;
            } else if (used_ + 1 < sizeof(line_)) {
                line_[used_++] = c;
            } else {
                used_ = 0;
                Serial.println("Команда слишком длинная — отброшена");
            }
        }
    }

    bool wifiRequested() const { return wifiRequested_; }
    void clearWifiRequest()     { wifiRequested_ = false; }

private:
    char   line_[128];
    size_t used_ = 0;
    bool   wifiRequested_ = false;

    void handleLine(char* line) {
        while (*line && isspace((unsigned char)*line)) ++line;
        size_t len = strlen(line);
        while (len && isspace((unsigned char)line[len - 1])) line[--len] = '\0';
        if (!len) return;

        if (!strcasecmp(line, "help"))            { printHelp(); return; }
        if (!strcasecmp(line, "show"))            { printAll(); return; }
        if (!strcasecmp(line, "save"))            { syncParams(true); return; }
        if (!strcasecmp(line, "wifi"))            { wifiRequested_ = true; return; }
        if (!strcasecmp(line, "reboot"))          { syncParams(true); Serial.println("Перезагрузка..."); delay(300); ESP.restart(); return; }
        if (!strcasecmp(line, "reset"))           {
            setDefaults();
            syncParams(true);
            Serial.println("Параметры сброшены к значениям по умолчанию. Перезагрузка...");
            delay(300);
            ESP.restart();
            return;
        }

        if (!strncasecmp(line, "set ", 4)) {
            char* key = line + 4;
            char* sep = strchr(key, ' ');
            if (!sep) { Serial.println("Формат: set <ключ> <значение>"); return; }
            *sep++ = '\0';
            while (*sep && isspace((unsigned char)*sep)) ++sep;
            trimUnquote(sep);
            if (setParam(key, sep)) {
                Serial.printf("'%s' обновлён. Сохранение в NVS — в течение секунды; "
                              "сетевые настройки и порты применяются после 'reboot'.\n", key);
            } else {
                Serial.printf("Неверный ключ или значение: %s\n", key);
            }
            return;
        }

        Serial.println("Неизвестная команда. Введите 'help'.");
    }

    void printHelp() {
        Serial.println("Команды:");
        Serial.println("  show                       показать все параметры");
        Serial.println("  set <ключ> <значение>      изменить параметр (значения с пробелами — в кавычках)");
        Serial.println("  save | reboot | reset | wifi");
        Serial.println("Ключи:");
        Serial.println("  wifi_ssid, wifi_pass, wifi_type (auto|compat|standard)");
        Serial.println("  mac, ip (dhcp|адрес), gateway, subnet, dns (none|адрес)");
        Serial.println("  port_sim1, port_sim2, port_out1, port_out2");
        Serial.println("  log_level (error|warn|info|debug)");
        Serial.println("  modem1_type, modem2_type (standard|no_ok_on_ath|slow|no_ok_on_escape)");
        Serial.println("  idle_timeout (мс простоя вызова до разрыва, минимум 10000)");
        Serial.println("  director_en (on|off) — эта коробка управляет парком");
        Serial.println("  director_port, pool_port, director_ip (none|адрес), director_auto (on|off)");
        Serial.println("  box_name — имя коробки в парке");
    }

    static const char* ipText(const char* s) {
        return (!s[0] || !strcmp(s, "0.0.0.0")) ? "DHCP" : s;
    }

    void printAll() {
        Serial.println("\n--- Параметры шлюза (NVS) ---");
        Serial.printf("wifi_ssid    : %s\n", params.wifiSsid[0] ? params.wifiSsid : "<не задан>");
        Serial.printf("wifi_pass    : %s\n", params.wifiPass[0] ? "********" : "<не задан>");
        Serial.printf("wifi_type    : %u (0=auto, 1=compat, 2=standard)\n", params.wifiType);
        Serial.printf("mac          : %s\n", params.mac[0] ? params.mac : "<аппаратный>");
        Serial.printf("ip           : %s\n", ipText(params.ip));
        Serial.printf("gateway      : %s\n", params.gateway);
        Serial.printf("subnet       : %s\n", params.subnet);
        Serial.printf("dns          : %s\n", params.dns);
        Serial.printf("port_sim1    : %u\n", params.portSim1);
        Serial.printf("port_sim2    : %u\n", params.portSim2);
        Serial.printf("port_out1    : %u (справочный NAT-порт)\n", params.portOut1);
        Serial.printf("port_out2    : %u (справочный NAT-порт)\n", params.portOut2);
        Serial.printf("log_level    : %u (0=error, 1=warn, 2=info, 3=debug)\n", params.logLevel);
        Serial.printf("modem1_type  : %u (0=standard, 1=no_ok_on_ath, 2=slow, 3=no_ok_on_escape)\n", params.modem1Type);
        Serial.printf("modem2_type  : %u (0=standard, 1=no_ok_on_ath, 2=slow, 3=no_ok_on_escape)\n", params.modem2Type);
        Serial.printf("idle_timeout : %u мс\n", params.idleTimeoutMs);
        Serial.println("--- парк коробок ---");
        Serial.printf("director_en  : %u (1 = эта коробка дирижёр)\n", params.directorEn);
        Serial.printf("director_port: %u\n", params.directorPort);
        Serial.printf("pool_port    : %u\n", params.poolPort);
        Serial.printf("director_ip  : %s\n",
                      params.directorIp[0] ? params.directorIp
                                           : (params.directorAuto ? "<автопоиск>" : "<не задан>"));
        Serial.printf("director_auto: %u\n", params.directorAuto);
        Serial.printf("box_name     : %s\n", params.boxName[0] ? params.boxName : "<авто из MAC>");
        Serial.println("-----------------------------\n");
    }
};

static Console console;

} // namespace gp
