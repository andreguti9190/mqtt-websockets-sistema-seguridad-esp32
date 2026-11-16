#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>

// Usa un GPIO libre de tu placa. GPIO 26 funciona en la mayoría de ESP32 DevKit.
const uint8_t RELAY_PIN = 26;
const bool RELAY_ACTIVE_LOW = true; // Muchos módulos relay se activan llevando IN a LOW.
const uint8_t CONFIG_BUTTON_PIN = 27; // Botón entre GPIO 27 y GND
const uint8_t STATUS_LED_PIN = 2; // LED integrado controlable en la mayoría de ESP32 DevKit.
const bool STATUS_LED_ACTIVE_HIGH = true;
const char *CONFIG_AP_SSID = "Cerrojo-Config";
const char *CONFIG_AP_PASSWORD = "cerrojo123"; // Mínimo 8 caracteres
const unsigned long CONFIG_HOLD_MS = 15000;
const unsigned long DELETE_HOLD_MS = 7000;
const unsigned long ENROLL_BUTTON_MAX_MS = 2000;
const IPAddress CONFIG_AP_IP(192, 168, 4, 1);
const unsigned long WIFI_RETRY_MS = 10000;

// RC522 por VSPI. En el RC522, el pin rotulado SDA funciona como SS/CS.
const uint8_t RFID_SS_PIN = 21;
const uint8_t RFID_RST_PIN = 22;
const uint8_t RFID_SCK_PIN = 18;
const uint8_t RFID_MISO_PIN = 19;
const uint8_t RFID_MOSI_PIN = 23;
const unsigned long RFID_UNLOCK_MS = 5000;
const unsigned long RFID_ENROLLMENT_MS = 45000;
const unsigned long RFID_REPEAT_GUARD_MS = 800;
const uint8_t MAX_RFID_CARDS = 20;

const char *COMMAND_TOPIC = "actuador/cerrojo";
const char *STATE_TOPIC = "sensor/cerrojo";
const char *RFID_EVENT_TOPIC = "evento/cerrojo/rfid";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer configServer(80);
DNSServer dnsServer;
Preferences preferences;
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
String wifiSsid;
String wifiPassword;
String mqttBroker;
String wifiNetworkOptions;
uint16_t mqttPort = 1883;
bool isClosed = false;
unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;
bool configMode = false;
unsigned long configButtonPressedAt = 0;
bool configButtonLongPressHandled = false;
bool deleteModeTriggeredByHold = false;
bool enrollmentMode = false;
unsigned long enrollmentEndsAt = 0;
bool deletionMode = false;
unsigned long deletionEndsAt = 0;
bool rfidUnlockActive = false;
unsigned long rfidRelockAt = 0;
String lastRfidUid;
unsigned long lastRfidReadAt = 0;
unsigned long ledFeedbackEndsAt = 0;
unsigned long ledFeedbackInterval = 0;
bool relayStateKnown = false;
bool relayEnergized = false;
bool rfidRecoveryPending = false;
unsigned long rfidRecoveryAt = 0;
uint8_t rfidRecoveryAttempts = 0;

struct StoredCard {
  uint8_t length;
  byte uid[10];
};

StoredCard authorizedCards[MAX_RFID_CARDS];
uint8_t authorizedCardCount = 0;

void setRelay(bool energized)
{
  const uint8_t onLevel = RELAY_ACTIVE_LOW ? LOW : HIGH;
  const uint8_t offLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;
  const bool changed = relayStateKnown && relayEnergized != energized;
  digitalWrite(RELAY_PIN, energized ? onLevel : offLevel);
  relayEnergized = energized;
  relayStateKnown = true;

  // La bobina del rele/cerradura puede generar una interferencia breve que
  // deja algunos RC522 clonados sin responder. Revalidamos el lector cuando
  // el ruido ya paso, sin bloquear el callback de MQTT.
  if (changed) {
    rfidRecoveryPending = true;
    rfidRecoveryAt = millis() + 250;
    rfidRecoveryAttempts = 0;
  }
}

void setStatusLed(bool on)
{
  digitalWrite(STATUS_LED_PIN, on == STATUS_LED_ACTIVE_HIGH ? HIGH : LOW);
}

void showLedFeedback(unsigned long durationMs, unsigned long intervalMs)
{
  ledFeedbackEndsAt = millis() + durationMs;
  ledFeedbackInterval = intervalMs;
}

void updateStatusLed()
{
  bool ledOn = false;

  if ((long)(ledFeedbackEndsAt - millis()) > 0) {
    ledOn = ((millis() / ledFeedbackInterval) % 2) == 0;
  } else if (configMode) {
    ledOn = ((millis() / 350) % 2) == 0;
  } else if (enrollmentMode) {
    ledOn = ((millis() / 100) % 2) == 0;
  } else if (deletionMode) {
    // Dos destellos cortos cada 1,2 segundos.
    const unsigned long phase = millis() % 1200;
    ledOn = phase < 100 || (phase >= 220 && phase < 320);
  } else if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) {
    ledOn = ((millis() / 700) % 2) == 0;
  } else {
    // Un pulso corto cada dos segundos confirma que el firmware sigue activo.
    ledOn = (millis() % 2000) < 80;
  }

  setStatusLed(ledOn);
}

bool initializeRfidReader(bool announce)
{
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
  delay(4);

  const byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  const bool ready = version != 0x00 && version != 0xFF;
  if (announce || !ready) {
    if (ready) {
      Serial.printf("RC522 listo (VersionReg=0x%02X).\n", version);
    } else {
      Serial.printf("ERROR RC522: VersionReg=0x%02X; revisa alimentacion y SPI.\n", version);
    }
  }
  return ready;
}

void checkRfidRecovery()
{
  if (!rfidRecoveryPending || (long)(millis() - rfidRecoveryAt) < 0) return;

  rfidRecoveryAttempts++;
  if (initializeRfidReader(false)) {
    rfidRecoveryPending = false;
    Serial.println("RC522 recuperado despues de conmutar el rele.");
    return;
  }

  if (rfidRecoveryAttempts >= 5) {
    rfidRecoveryPending = false;
    Serial.println("ERROR RC522: no se recupero despues de conmutar el rele.");
  } else {
    rfidRecoveryAt = millis() + 500;
  }
}

void publishState()
{
  if (mqtt.connected()) {
    mqtt.publish(STATE_TOPIC, isClosed ? "ON" : "OFF", true);
  }
}

String uidToString(const byte *uid, uint8_t length)
{
  String value;
  value.reserve(length * 2);
  char part[3];
  for (uint8_t i = 0; i < length; i++) {
    snprintf(part, sizeof(part), "%02X", uid[i]);
    value += part;
  }
  return value;
}

void publishRfidEvent(const String &event, const String &uid = "")
{
  String message = event;
  if (uid.length()) message += " uid=" + uid;
  Serial.printf("RFID: %s\n", message.c_str());
  if (mqtt.connected()) mqtt.publish(RFID_EVENT_TOPIC, message.c_str(), false);
}

int findAuthorizedCard(const byte *uid, uint8_t length)
{
  for (uint8_t i = 0; i < authorizedCardCount; i++) {
    if (authorizedCards[i].length == length &&
        memcmp(authorizedCards[i].uid, uid, length) == 0) {
      return i;
    }
  }
  return -1;
}

void loadAuthorizedCards()
{
  authorizedCardCount = 0;
  preferences.begin("lockcfg", true);
  const uint8_t storedCount = min(preferences.getUChar("cardCount", 0), MAX_RFID_CARDS);
  for (uint8_t i = 0; i < storedCount; i++) {
    String key = "card" + String(i);
    const size_t length = preferences.getBytesLength(key.c_str());
    if (length < 1 || length > sizeof(authorizedCards[0].uid)) continue;
    authorizedCards[authorizedCardCount].length = (uint8_t)length;
    preferences.getBytes(key.c_str(), authorizedCards[authorizedCardCount].uid, length);
    authorizedCardCount++;
  }
  preferences.end();
  Serial.printf("RFID: %u tarjeta(s) autorizada(s) cargada(s).\n", authorizedCardCount);
}

void saveAuthorizedCards()
{
  preferences.begin("lockcfg", false);
  const uint8_t previousCount = min(preferences.getUChar("cardCount", 0), MAX_RFID_CARDS);
  preferences.putUChar("cardCount", authorizedCardCount);

  for (uint8_t i = 0; i < authorizedCardCount; i++) {
    String key = "card" + String(i);
    preferences.putBytes(key.c_str(), authorizedCards[i].uid, authorizedCards[i].length);
  }
  for (uint8_t i = authorizedCardCount; i < previousCount; i++) {
    String key = "card" + String(i);
    preferences.remove(key.c_str());
  }
  preferences.end();
}

void startEnrollmentMode()
{
  deletionMode = false;
  enrollmentMode = true;
  enrollmentEndsAt = millis() + RFID_ENROLLMENT_MS;
  lastRfidUid = "";
  lastRfidReadAt = 0;
  initializeRfidReader(true);
  publishRfidEvent("MODO_ALTA_ACTIVO_45S");
  Serial.printf("RFID: acerca una tarjeta. Hay %u autorizada(s).\n", authorizedCardCount);
}

void startDeletionMode()
{
  enrollmentMode = false;
  deletionMode = true;
  deletionEndsAt = millis() + RFID_ENROLLMENT_MS;
  lastRfidUid = "";
  lastRfidReadAt = 0;
  initializeRfidReader(true);
  publishRfidEvent("MODO_BAJA_ACTIVO_45S");
  Serial.printf("RFID: acerca la tarjeta que deseas eliminar. Hay %u autorizada(s).\n", authorizedCardCount);
}

void openFromRfid(const String &uid)
{
  isClosed = false;
  setRelay(false);
  publishState();
  rfidUnlockActive = true;
  rfidRelockAt = millis() + RFID_UNLOCK_MS;
  showLedFeedback(1000, 80);
  publishRfidEvent("ACCESO_AUTORIZADO", uid);
}

void checkRfidRelock()
{
  if (!rfidUnlockActive || (long)(millis() - rfidRelockAt) < 0) return;

  rfidUnlockActive = false;
  isClosed = true;
  setRelay(true);
  publishState();
  publishRfidEvent("CIERRE_AUTOMATICO");
}

void processRfid()
{
  if (configMode || rfidRecoveryPending) return;

  // WUPA también despierta tarjetas que quedaron en HALT después de una lectura
  // anterior. Esto hace confiable volver a presentar la misma credencial.
  byte atqa[2];
  byte atqaSize = sizeof(atqa);
  const MFRC522::StatusCode wakeStatus = rfid.PICC_WakeupA(atqa, &atqaSize);
  if (wakeStatus != MFRC522::STATUS_OK && wakeStatus != MFRC522::STATUS_COLLISION) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  const String uid = uidToString(rfid.uid.uidByte, rfid.uid.size);
  if (uid == lastRfidUid && millis() - lastRfidReadAt < RFID_REPEAT_GUARD_MS) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    initializeRfidReader(false);
    return;
  }
  lastRfidUid = uid;
  lastRfidReadAt = millis();

  const char *mode = enrollmentMode ? "ALTA" : (deletionMode ? "BAJA" : "ACCESO");
  Serial.printf("RFID: TARJETA_DETECTADA uid=%s modo=%s\n", uid.c_str(), mode);

  const int cardIndex = findAuthorizedCard(rfid.uid.uidByte, rfid.uid.size);
  if (enrollmentMode) {
    enrollmentEndsAt = millis() + RFID_ENROLLMENT_MS;
    if (cardIndex >= 0) {
      showLedFeedback(700, 250);
      publishRfidEvent("TARJETA_YA_AUTORIZADA", uid);
    } else if (authorizedCardCount >= MAX_RFID_CARDS) {
      showLedFeedback(1800, 60);
      publishRfidEvent("LIMITE_DE_TARJETAS", uid);
    } else if (rfid.uid.size > sizeof(authorizedCards[0].uid)) {
      showLedFeedback(1800, 60);
      publishRfidEvent("UID_NO_COMPATIBLE", uid);
    } else {
      authorizedCards[authorizedCardCount].length = rfid.uid.size;
      memcpy(authorizedCards[authorizedCardCount].uid, rfid.uid.uidByte, rfid.uid.size);
      authorizedCardCount++;
      saveAuthorizedCards();
      showLedFeedback(1200, 100);
      publishRfidEvent("TARJETA_AGREGADA", uid);
    }
  } else if (deletionMode) {
    deletionEndsAt = millis() + RFID_ENROLLMENT_MS;
    if (cardIndex < 0) {
      showLedFeedback(700, 250);
      publishRfidEvent("TARJETA_NO_REGISTRADA", uid);
    } else {
      for (uint8_t i = cardIndex; i + 1 < authorizedCardCount; i++) {
        authorizedCards[i] = authorizedCards[i + 1];
      }
      authorizedCardCount--;
      saveAuthorizedCards();
      showLedFeedback(1200, 100);
      publishRfidEvent("TARJETA_ELIMINADA", uid);
    }
  } else if (cardIndex >= 0) {
    openFromRfid(uid);
  } else {
    showLedFeedback(1800, 60);
    publishRfidEvent("ACCESO_DENEGADO", uid);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  // Algunos módulos RC522 clonados quedan en un estado inestable después de
  // HALT. Una reinicialización corta deja la antena lista para la próxima tarjeta.
  initializeRfidReader(false);
}

void onMqttMessage(char *topic, byte *payload, unsigned int length)
{
  if (strcmp(topic, COMMAND_TOPIC) != 0) return;

  String command;
  command.reserve(length);
  for (unsigned int i = 0; i < length; i++) command += (char)payload[i];
  command.trim();
  command.toUpperCase();

  if (command == "CERRAR") {
    rfidUnlockActive = false;
    isClosed = true;
    setRelay(true);
    publishState();
    Serial.println("Comando recibido: CERRAR");
  } else if (command == "ABRIR") {
    rfidUnlockActive = false;
    isClosed = false;
    setRelay(false);
    publishState();
    Serial.println("Comando recibido: ABRIR");
  } else {
    Serial.printf("Comando desconocido: %s\n", command.c_str());
  }
}

void connectMqttIfNeeded()
{
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttempt < 3000) return;
  lastMqttAttempt = millis();

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "ESP32-cerrojo-%llX", ESP.getEfuseMac());

  if (mqtt.connect(clientId)) {
    mqtt.subscribe(COMMAND_TOPIC);
    publishState();
    Serial.println("MQTT conectado; escuchando actuador/cerrojo");
  } else {
    Serial.printf("Fallo MQTT, estado=%d; reintentando\n", mqtt.state());
  }
}

String escapeHtml(String value)
{
  value.replace("&", "&amp;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  return value;
}

void scanWifiNetworks()
{
  wifiNetworkOptions = "";
  const int networkCount = WiFi.scanNetworks(false, false);

  if (networkCount <= 0) {
    wifiNetworkOptions = F("<option value='' disabled selected>No se encontraron redes</option>");
    WiFi.scanDelete();
    Serial.println("Escaneo WiFi: no se encontraron redes.");
    return;
  }

  for (int i = 0; i < networkCount; i++) {
    const String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue; // No mostrar redes ocultas: no pueden seleccionarse por nombre.

    wifiNetworkOptions += F("<option value='");
    wifiNetworkOptions += escapeHtml(ssid);
    wifiNetworkOptions += "'";
    if (ssid == wifiSsid) wifiNetworkOptions += F(" selected");
    wifiNetworkOptions += ">";
    wifiNetworkOptions += escapeHtml(ssid);
    wifiNetworkOptions += " (";
    wifiNetworkOptions += String(WiFi.RSSI(i));
    wifiNetworkOptions += F(" dBm, ");
    if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
      wifiNetworkOptions += F("abierta");
    } else {
      wifiNetworkOptions += F("con clave");
    }
    wifiNetworkOptions += F(")</option>");
  }

  WiFi.scanDelete();
  Serial.printf("Escaneo WiFi: %d redes detectadas.\n", networkCount);
}

String configPage(const String &message = "")
{
  String page = F("<!doctype html><html lang='es'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<meta charset='utf-8'><title>Configurar cerrojo</title>"
                  "<style>body{font:16px sans-serif;max-width:480px;margin:32px auto;padding:0 16px}"
                  "input,select{box-sizing:border-box;width:100%;padding:10px;margin:6px 0 14px}"
                  "button{padding:12px 18px}.aviso{background:#eef4ff;padding:12px;border-radius:6px}</style>"
                  "<h1>Configurar cerrojo</h1>"
                  "<p class='aviso'>Usa la IP local de la computadora donde corre el broker MQTT. No uses localhost.</p>");
  if (message.length()) page += "<p>" + message + "</p>";
  page += F("<form method='post' action='/save'>"
            "<label>Red Wi-Fi detectada</label><select name='ssid' required>");
  page += wifiNetworkOptions;
  page += F("</select><p><a href='/scan'>Volver a buscar redes</a></p>"
            "<label>Clave de Wi-Fi</label>"
            "<input name='password' type='password' maxlength='64' placeholder='Dejar vacia para conservarla'>"
            "<label>IP o nombre del broker MQTT</label><input name='broker' required maxlength='64' value='");
  page += escapeHtml(mqttBroker);
  page += F("'><label>Puerto MQTT</label><input name='port' type='number' min='1' max='65535' value='");
  page += String(mqttPort);
  page += F("' required><button type='submit'>Guardar y reiniciar</button></form>"
            "<p>Si el portal no se abre solo, visita <strong>http://192.168.4.1</strong>.</p></html>");
  return page;
}

void startConfigMode()
{
  if (configMode) return;
  configMode = true;
  enrollmentMode = false;
  deletionMode = false;
  mqtt.disconnect();
  WiFi.disconnect(true);
  delay(100);
  // AP+STA permite mantener abierto el portal mientras el ESP32 busca redes cercanas.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(CONFIG_AP_IP, CONFIG_AP_IP, IPAddress(255, 255, 255, 0));

  if (!WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD)) {
    Serial.println("ERROR: no se pudo crear la red de configuracion.");
    return;
  }

  scanWifiNetworks();

  configServer.on("/", HTTP_GET, []() {
    configServer.send(200, "text/html; charset=utf-8", configPage());
  });
  configServer.on("/scan", HTTP_GET, []() {
    scanWifiNetworks();
    configServer.send(200, "text/html; charset=utf-8", configPage());
  });
  configServer.on("/save", HTTP_POST, []() {
    String ssid = configServer.arg("ssid");
    String password = configServer.arg("password");
    String broker = configServer.arg("broker");
    ssid.trim();
    broker.trim();
    long port = configServer.arg("port").toInt();
    if (!ssid.length() || !broker.length() || port < 1 || port > 65535) {
      configServer.send(400, "text/html; charset=utf-8", configPage("Revisa los datos ingresados."));
      return;
    }
    // Si no se cambia el SSID, una clave vacia conserva la clave almacenada.
    if (!password.length() && ssid == wifiSsid) password = wifiPassword;
    preferences.begin("lockcfg", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putString("broker", broker);
    preferences.putUShort("port", (uint16_t)port);
    preferences.end();
    configServer.send(200, "text/html; charset=utf-8",
      "<meta charset='utf-8'><h1>Guardado</h1><p>El ESP32 se reiniciara para conectar con la nueva configuracion.</p>");
    delay(1500);
    ESP.restart();
  });

  // Rutas consultadas por Android, iOS y Windows al detectar un portal cautivo.
  configServer.on("/generate_204", HTTP_ANY, []() {
    configServer.sendHeader("Location", "http://192.168.4.1/", true);
    configServer.send(302, "text/plain", "");
  });
  configServer.on("/gen_204", HTTP_ANY, []() {
    configServer.sendHeader("Location", "http://192.168.4.1/", true);
    configServer.send(302, "text/plain", "");
  });
  configServer.on("/hotspot-detect.html", HTTP_ANY, []() {
    configServer.sendHeader("Location", "http://192.168.4.1/", true);
    configServer.send(302, "text/plain", "");
  });
  configServer.on("/connecttest.txt", HTTP_ANY, []() {
    configServer.sendHeader("Location", "http://192.168.4.1/", true);
    configServer.send(302, "text/plain", "");
  });
  configServer.on("/ncsi.txt", HTTP_ANY, []() {
    configServer.sendHeader("Location", "http://192.168.4.1/", true);
    configServer.send(302, "text/plain", "");
  });
  configServer.onNotFound([]() {
    configServer.sendHeader("Location", "http://192.168.4.1/", true);
    configServer.send(302, "text/plain", "");
  });

  dnsServer.start(53, "*", CONFIG_AP_IP);
  configServer.begin();
  Serial.println("\n=== MODO CONFIGURACION ===");
  Serial.printf("1. Conectate a: %s\n", CONFIG_AP_SSID);
  Serial.printf("2. Clave: %s\n", CONFIG_AP_PASSWORD);
  Serial.println("3. Abre: http://192.168.4.1");
}

void checkConfigButton()
{
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    if (configButtonPressedAt == 0) {
      configButtonPressedAt = millis();
      configButtonLongPressHandled = false;
      deleteModeTriggeredByHold = false;
      Serial.println("Boton: <2 s alta, 7 s baja, 15 s configuracion.");
    }
    if (!deleteModeTriggeredByHold && millis() - configButtonPressedAt >= DELETE_HOLD_MS) {
      deleteModeTriggeredByHold = true;
      startDeletionMode();
    }
    if (!configButtonLongPressHandled && millis() - configButtonPressedAt >= CONFIG_HOLD_MS) {
      configButtonLongPressHandled = true;
      deletionMode = false;
      startConfigMode();
    }
  } else {
    if (!configMode && configButtonPressedAt != 0 && !configButtonLongPressHandled &&
        !deleteModeTriggeredByHold) {
      const unsigned long pressedFor = millis() - configButtonPressedAt;
      if (pressedFor >= 50 && pressedFor <= ENROLL_BUTTON_MAX_MS) {
        startEnrollmentMode();
      } else if (pressedFor > ENROLL_BUTTON_MAX_MS) {
        Serial.println("Pulsacion ignorada. Usa <2 s para alta, 7 s para baja o 15 s para configurar.");
      }
    }
    configButtonPressedAt = 0;
    configButtonLongPressHandled = false;
    deleteModeTriggeredByHold = false;
  }
}

void connectWifiIfNeeded()
{
  if (configMode || WiFi.status() == WL_CONNECTED) return;
  if (lastWifiAttempt != 0 && millis() - lastWifiAttempt < WIFI_RETRY_MS) return;

  lastWifiAttempt = millis();
  mqtt.disconnect();
  Serial.printf("Reconectando a WiFi '%s'...\n", wifiSsid.c_str());
  WiFi.disconnect();
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
}

void setup()
{
  Serial.begin(115200);

  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_LED_PIN, OUTPUT);
  setStatusLed(false);

  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  initializeRfidReader(true);
  loadAuthorizedCards();

  // Inicializa el relay apagado para evitar un accionamiento durante el arranque.
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

  preferences.begin("lockcfg", true);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  mqttBroker = preferences.getString("broker", "");
  mqttPort = preferences.getUShort("port", 1883);
  preferences.end();

  if (!wifiSsid.length() || !mqttBroker.length()) {
    startConfigMode();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.print("Conectando al WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    checkConfigButton();
    updateStatusLed();
    if (configMode) return;
    delay(25);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nNo se pudo conectar. Iniciando configuracion.");
    startConfigMode();
    return;
  }
  Serial.printf("\nWiFi conectado. IP: %s\n", WiFi.localIP().toString().c_str());

  mqtt.setServer(mqttBroker.c_str(), mqttPort);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(128);
}

void loop()
{
  checkConfigButton();
  updateStatusLed();
  if (configMode) {
    dnsServer.processNextRequest();
    configServer.handleClient();
    delay(2);
    return;
  }

  if (enrollmentMode && (long)(millis() - enrollmentEndsAt) >= 0) {
    enrollmentMode = false;
    publishRfidEvent("MODO_ALTA_FINALIZADO");
  }
  if (deletionMode && (long)(millis() - deletionEndsAt) >= 0) {
    deletionMode = false;
    publishRfidEvent("MODO_BAJA_FINALIZADO");
  }

  connectWifiIfNeeded();
  connectMqttIfNeeded();
  mqtt.loop();
  checkRfidRecovery();
  processRfid();
  checkRfidRelock();
  delay(2);
}
