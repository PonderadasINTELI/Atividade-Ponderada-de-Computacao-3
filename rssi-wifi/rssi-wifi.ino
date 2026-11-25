// Le o RSSI do Wi-Fi (dBm), mostra no Serial e publica o mesmo valor via MQTT.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "secrets.h"

const unsigned long INTERVALO_MS = 2000;  // 5 amostras = 10 s

#if MQTT_TLS
// Autoridades publicas que ja vem no core do ESP32: validam EMQX Cloud, HiveMQ Cloud etc.
extern const uint8_t caBundle[] asm("_binary_x509_crt_bundle_start");
WiFiClientSecure rede;
#else
WiFiClient rede;
#endif
PubSubClient mqtt(rede);

unsigned long ultimaLeitura = 0, ultimaTentativa = 0, seq = 0;

void aoDesconectar(WiFiEvent_t, WiFiEventInfo_t info) {
  Serial.printf("[wifi] SEM CONEXAO (motivo %d), reconectando...\n", info.wifi_sta_disconnected.reason);
}
void aoConectar(WiFiEvent_t, WiFiEventInfo_t) {
  // BSSID: se mudar entre testes, o ESP32 trocou de roteador e a comparacao nao vale.
  Serial.printf("[wifi] conectado, IP %s, roteador %s, canal %d\n",
                WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str(), WiFi.channel());
}

void setup() {
  Serial.begin(115200);
  WiFi.onEvent(aoDesconectar, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent(aoConectar, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  // Se reiniciar com "Brownout detector was triggered", ative: reduz o pico de corrente da
  // transmissao e nao altera o RSSI, que mede o sinal recebido.
  // WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#if MQTT_TLS
  rede.setCACertBundle(caBundle);
  rede.setHandshakeTimeout(8);
#endif
  rede.setTimeout(5);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(5);
  mqtt.setKeepAlive(15);
}

void manterMqtt() {
  if (mqtt.connected()) { mqtt.loop(); return; }
  if (WiFi.status() != WL_CONNECTED || millis() - ultimaTentativa < 3000) return;
  bool ok = strlen(MQTT_USER) ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)
                              : mqtt.connect(MQTT_CLIENT_ID);
  ultimaTentativa = millis();
  Serial.printf(ok ? "[mqtt] conectado\n" : "[mqtt] falhou, codigo %d\n", mqtt.state());
}

void loop() {
  manterMqtt();
  if (millis() - ultimaLeitura < INTERVALO_MS) return;
  ultimaLeitura = millis();

  int rssi = WiFi.RSSI();
  if (WiFi.status() != WL_CONNECTED || rssi >= 0) {  // sem Wi-Fi nao existe RSSI valido
    Serial.println("[rssi] sem leitura: Wi-Fi desconectado");
    return;
  }
  char msg[64];
  snprintf(msg, sizeof(msg), "{\"rssi\":%d,\"seq\":%lu}", rssi, ++seq);
  bool ok = mqtt.connected() && mqtt.publish(MQTT_TOPIC, msg);
  Serial.printf("[rssi] %d dBm  amostra %lu  %s\n", rssi, seq, ok ? "publicada" : "NAO publicada");
}
