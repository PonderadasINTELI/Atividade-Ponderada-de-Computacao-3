// Le o RSSI do Wi-Fi (dBm), mostra no Serial e no LCD e publica o mesmo valor via MQTT.
// Sequencia: Trocando, modo 1, Trocando, modo 2, ... AVANCAR anda um passo e VOLTAR desfaz um.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
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

// Pinos da montagem antiga do prototipo do g01 (bancada-antiga). Na alternada, o SCL e 23.
// VOLTAR: 26 na montagem antiga e 32 na alternada; os dois valem, e nenhum tem outro uso.
const int PINO_AVANCAR = 25, PINO_VOLTAR = 26, PINO_VOLTAR_ALT = 32, LCD_SDA = 21, LCD_SCL = 22, LCD_ENDERECO = 0x27;
const unsigned long DEBOUNCE_MS = 50, ENTRE_TOQUES_MS = 300;  // um toque com mau contato nao conta duas vezes
LiquidCrystal_I2C lcd(LCD_ENDERECO, 16, 2);

// Mesma ordem de CONDICOES no dashboard/server.js. Ate 16 letras, sem acento. 0 = nao conta.
const char *const MODOS[] = {"Trocando", "Fora metal antes", "Dentro do metal", "Depois do metal", "Descoberta",
                             "Coberta c/ maos", "Vertical", "Horizontal", "Perto roteador", "Longe roteador"};
const int ULTIMO = sizeof(MODOS) / sizeof(MODOS[0]) - 1;

// O painel (botoes e LCD) roda em outra tarefa: a reconexao MQTT trava o loop por segundos e os
// toques se perderiam. A tarefa escreve modo; o loop escreve o resto.
volatile int modo = 0, rssiAtual = 0, amostrasModo = 0;
volatile bool mqttOk = false;
volatile int comando = -1;  // modo pedido pelo dashboard, aplicado pela tarefa painel; -1 = nenhum
volatile bool recomecar = false;  // dashboard zerou os testes: volta ao primeiro passo

// Antes da primeira funcao: a IDE do Arduino gera os prototipos naquele ponto.
struct Botao { int pino; bool bruto, apertado; unsigned long mudou; };

unsigned long ultimaLeitura = 0, ultimaTentativa = 0, seq = 0;

void aoDesconectar(WiFiEvent_t, WiFiEventInfo_t info) {
  Serial.printf("[wifi] SEM CONEXAO (motivo %d), reconectando...\n", info.wifi_sta_disconnected.reason);
}
void aoConectar(WiFiEvent_t, WiFiEventInfo_t) {
  // BSSID: se mudar entre testes, o ESP32 trocou de roteador e a comparacao nao vale.
  Serial.printf("[wifi] conectado, IP %s, roteador %s, canal %d\n",
                WiFi.localIP().toString().c_str(), WiFi.BSSIDstr().c_str(), WiFi.channel());
}

// true uma vez por toque, depois de DEBOUNCE_MS com a leitura parada.
bool tocou(Botao &b) {
  bool agora = digitalRead(b.pino) == LOW;
  if (agora != b.bruto) { b.bruto = agora; b.mudou = millis(); }
  if (b.bruto == b.apertado || millis() - b.mudou < DEBOUNCE_MS) return false;
  b.apertado = b.bruto;
  return b.apertado;
}

void escreveLinha(int linha, const char *texto, char *mostrado) {
  char t[17];
  snprintf(t, sizeof(t), "%-16.16s", texto);
  if (strcmp(t, mostrado) == 0) return;
  strcpy(mostrado, t);
  lcd.setCursor(0, linha);
  lcd.print(t);
}

void painel(void *) {
  Botao avancar{PINO_AVANCAR, false, false, 0}, voltar{PINO_VOLTAR, false, false, 0},
      voltarAlt{PINO_VOLTAR_ALT, false, false, 0};
  // Os demais botoes do prototipo so avisam no Serial em qual pino estao (diagnostico).
  Botao outros[] = {{33, false, false, 0}, {14, false, false, 0}, {18, false, false, 0}, {19, false, false, 0},
                    {27, false, false, 0}, {15, false, false, 0}, {5, false, false, 0}};
  for (int pino : {PINO_AVANCAR, PINO_VOLTAR, PINO_VOLTAR_ALT}) pinMode(pino, INPUT_PULLUP);
  for (Botao &b : outros) pinMode(b.pino, INPUT_PULLUP);
  Wire.begin(LCD_SDA, LCD_SCL);  // antes do init(): a biblioteca chama Wire.begin() sem pinos
  Wire.beginTransmission(LCD_ENDERECO);
  Serial.println(Wire.endTransmission() == 0 ? "[lcd] encontrado" : "[lcd] NAO respondeu: confira SDA 21, SCL 22");
  lcd.init();
  Wire.setClock(50000);  // fios longos na protoboard corrompem o I2C na velocidade padrao
  lcd.backlight();

  char l1[17] = "", l2[17] = "", t[24];
  const int FIM = 2 * ULTIMO;
  int passo = 0;  // impar = dentro de um modo; par = Trocando antes do proximo
  unsigned long ultimoToque = 0;
  for (;;) {
    bool av = tocou(avancar), v1 = tocou(voltar), v2 = tocou(voltarAlt), vo = v1 || v2;
    if (vo) Serial.printf("[botao] VOLTAR no GPIO%d\n", v1 ? PINO_VOLTAR : PINO_VOLTAR_ALT);
    for (Botao &b : outros) if (tocou(b)) Serial.printf("[botao] toque no GPIO%d, sem funcao\n", b.pino);
    if ((av || vo) && millis() - ultimoToque > ENTRE_TOQUES_MS) {
      ultimoToque = millis();
      if (av && passo < FIM) passo++;
      else if (vo && !av && passo > 0) passo--;
      modo = passo % 2 ? passo / 2 + 1 : 0;
      Serial.printf("[modo] %s (passo %d de %d)\n", MODOS[modo], passo, FIM);
    }
    if (recomecar) {
      recomecar = false;
      comando = -1;
      passo = 0;
      modo = 0;
      Serial.println("[modo] testes zerados pelo dashboard");
    }
    if (comando >= 0) {
      int c = comando;
      comando = -1;
      if (c) passo = 2 * c - 1;
      else if (passo % 2) passo++;  // Trocando: mesmo efeito do AVANCAR dentro de um modo
      modo = c;
      Serial.printf("[modo] %s (pelo dashboard, passo %d de %d)\n", MODOS[modo], passo, FIM);
    }

    bool wifi = WiFi.status() == WL_CONNECTED;
    escreveLinha(0, modo ? MODOS[modo] : passo == FIM ? "Fim dos modos" : wifi ? "Trocando. Prox.:" : "SEM WIFI  Prox.:", l1);
    if (!modo) escreveLinha(1, passo == FIM ? "VOLTAR: repetir" : MODOS[passo / 2 + 1], l2);
    else {
      if (!wifi) strcpy(t, "SEM WIFI");
      else if (!mqttOk) snprintf(t, sizeof(t), "%d dBm SEM MQTT", rssiAtual);
      else snprintf(t, sizeof(t), "%d dBm  n=%d", rssiAtual, amostrasModo);
      escreveLinha(1, t, l2);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Comandos do dashboard em MQTT_TOPIC/cmd: {"modo":N} ou {"reset":1}. Fora da faixa, ignora.
void aoReceber(char *, byte *dados, unsigned int tam) {
  char t[32];
  if (tam >= sizeof(t)) return;
  memcpy(t, dados, tam);
  t[tam] = 0;
  if (strcmp(t, "{\"reset\":1}") == 0) { recomecar = true; return; }
  int m = -1;
  if (sscanf(t, "{\"modo\":%d}", &m) == 1 && m >= 0 && m <= ULTIMO) comando = m;
}

void setup() {
  Serial.begin(115200);
  xTaskCreatePinnedToCore(painel, "painel", 4096, nullptr, 1, nullptr, 0);
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
  mqtt.setCallback(aoReceber);
  mqtt.setSocketTimeout(5);
  mqtt.setKeepAlive(15);
}

void manterMqtt() {
  mqttOk = mqtt.connected();
  if (mqttOk) { mqtt.loop(); return; }
  if (WiFi.status() != WL_CONNECTED || millis() - ultimaTentativa < 3000) return;
  bool ok = strlen(MQTT_USER) ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)
                              : mqtt.connect(MQTT_CLIENT_ID);
  ultimaTentativa = millis();
  mqttOk = ok;
  if (ok && !mqtt.subscribe(MQTT_TOPIC "/cmd")) Serial.println("[mqtt] falha ao assinar comandos");
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
  static int modoAnterior = 0;
  const int m = modo;
  if (m != modoAnterior) { modoAnterior = m; amostrasModo = 0; }
  rssiAtual = rssi;
  char msg[64];
  snprintf(msg, sizeof(msg), "{\"rssi\":%d,\"seq\":%lu,\"modo\":%d}", rssi, ++seq, m);
  bool ok = mqtt.connected() && mqtt.publish(MQTT_TOPIC, msg);
  if (ok) amostrasModo = amostrasModo + 1;
  Serial.printf("[rssi] %d dBm  amostra %lu  [%s]  %s\n", rssi, seq, MODOS[m], ok ? "publicada" : "NAO publicada");
}
