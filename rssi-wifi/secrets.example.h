#pragma once
// Copie para secrets.h e preencha. O ESP32 e o dashboard leem o secrets.h.
// O secrets.h e ignorado pelo Git: nunca coloque senhas neste exemplo.

// Wi-Fi de 2,4 GHz com internet, sem login por navegador.
#define WIFI_SSID "PREENCHER"
#define WIFI_PASS "PREENCHER"

// Broker. Nuvem (EMQX Cloud): MQTT_TLS 1 e porta 8883. Local sem TLS: MQTT_TLS 0 e porta 1883.
#define MQTT_HOST "PREENCHER"
#define MQTT_PORT 8883
#define MQTT_TLS 1
#define MQTT_USER ""  // vazio = sem login
#define MQTT_PASS ""

// Topico e nomes unicos, para nao colidir com os do prototipo.
#define MQTT_TOPIC "rssi-wifi/esp32-39"
#define MQTT_CLIENT_ID "rssi-esp32-39"
