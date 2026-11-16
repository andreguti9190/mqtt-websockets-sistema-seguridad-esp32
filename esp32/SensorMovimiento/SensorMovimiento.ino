#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// Conecta OUT del sensor PIR a GPIO 26 y comparte GND con el ESP32.
const uint8_t PIR_PIN = 26;
// Botón de configuración entre GPIO 27 y GND.
const uint8_t CONFIG_BUTTON_PIN = 27;
const unsigned long CONFIG_HOLD_MS = 3000;

const char *CONFIG_AP_SSID = "Movimiento-Config";
const char *CONFIG_AP_PASSWORD = "movimiento123";
const IPAddress CONFIG_AP_IP(192, 168, 4, 1);
const unsigned long WIFI_RETRY_MS = 10000;
const char *MOTION_TOPIC = "sensor/movimiento";
const char *AVAILABILITY_TOPIC = "sensor/movimiento/disponibilidad";
const unsigned long PIR_DEBOUNCE_MS = 100;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer configServer(80);
DNSServer dnsServer;
Preferences preferences;

String wifiSsid;
String wifiPassword;
String mqttBroker;
String wifiNetworkOptions;
uint16_t mqttPort = 1883;

bool configMode = false;
bool motionState = false;
bool statePublished = false;
bool pirHasChanged = false;
bool rawPirState = false;
bool stablePirState = false;
unsigned long lastMqttAttempt = 0;
unsigned long lastWifiAttempt = 0;
unsigned long configButtonPressedAt = 0;
unsigned long rawPirChangedAt = 0;

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
    if (!ssid.length()) continue;

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
  String page = F("<!doctype html><html lang='es'><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>Configurar sensor de movimiento</title>"
                  "<style>body{font:16px sans-serif;max-width:480px;margin:32px auto;padding:0 16px}"
                  "input,select{box-sizing:border-box;width:100%;padding:10px;margin:6px 0 14px}"
                  "button{padding:12px 18px}.aviso{background:#eef4ff;padding:12px;border-radius:6px}</style>"
                  "<h1>Sensor de movimiento</h1>"
                  "<p class='aviso'>Usa la IP local de la computadora donde corre el broker MQTT. No uses localhost.</p>");
  if (message.length()) page += "<p>" + message + "</p>";
  page += F("<form method='post' action='/save'>"
            "<label>Red Wi-Fi detectada</label><select name='ssid' required>");
  page += wifiNetworkOptions;
  page += F("</select><p><a href='/scan'>Volver a buscar redes</a></p>"
            "<label>Clave del Wi-Fi</label>"
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
  mqtt.disconnect();
  WiFi.disconnect(true);
  delay(100);
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

    if (!password.length() && ssid == wifiSsid) password = wifiPassword;

    preferences.begin("motioncfg", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putString("broker", broker);
    preferences.putUShort("port", (uint16_t)port);
    preferences.end();

    configServer.send(200, "text/html; charset=utf-8",
      "<meta charset='utf-8'><h1>Configuracion guardada</h1><p>El ESP32 se reiniciara.</p>");
    delay(1500);
    ESP.restart();
  });

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
      Serial.println("Boton detectado; mantenlo presionado 3 segundos...");
    }
    if (millis() - configButtonPressedAt >= CONFIG_HOLD_MS) startConfigMode();
  } else {
    configButtonPressedAt = 0;
  }
}

void connectWifiIfNeeded()
{
  if (configMode || WiFi.status() == WL_CONNECTED) return;
  if (lastWifiAttempt != 0 && millis() - lastWifiAttempt < WIFI_RETRY_MS) return;

  lastWifiAttempt = millis();
  mqtt.disconnect();
  statePublished = false;
  Serial.printf("Reconectando a WiFi '%s'...\n", wifiSsid.c_str());
  WiFi.disconnect();
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
}

void connectMqttIfNeeded()
{
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttempt < 3000) return;
  lastMqttAttempt = millis();

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "ESP32-movimiento-%llX", ESP.getEfuseMac());
  if (mqtt.connect(clientId, AVAILABILITY_TOPIC, 0, true, "OFFLINE")) {
    mqtt.publish(AVAILABILITY_TOPIC, "ONLINE", true);
    Serial.println("MQTT conectado; publicando estado en sensor/movimiento");
    statePublished = false; // Publica el estado actual al reconectar.
  } else {
    Serial.printf("Error al conectar MQTT, estado=%d; se reintentara\n", mqtt.state());
  }
}

void updateMotionState()
{
  const bool currentRawState = digitalRead(PIR_PIN) == HIGH;
  if (currentRawState != rawPirState) {
    rawPirState = currentRawState;
    rawPirChangedAt = millis();
  }

  if (rawPirState != stablePirState && millis() - rawPirChangedAt >= PIR_DEBOUNCE_MS) {
    stablePirState = rawPirState;
    motionState = stablePirState;
    pirHasChanged = true;
    Serial.printf("PIR estable: %s\n", motionState ? "MOVIMIENTO (HIGH)" : "SIN MOVIMIENTO (LOW)");
  }
}

void publishMotionStateIfNeeded()
{
  if (!mqtt.connected() || (statePublished && !pirHasChanged)) return;
  const char *payload = motionState ? "ON" : "OFF";
  if (mqtt.publish(MOTION_TOPIC, payload, true)) {
    Serial.printf("Sensor de movimiento: %s\n", motionState ? "DETECTADO (ON)" : "SIN MOVIMIENTO (OFF)");
    statePublished = true;
    pirHasChanged = false;
  }
}

void setup()
{
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);
  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

  preferences.begin("motioncfg", true);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  mqttBroker = preferences.getString("broker", "");
  mqttPort = preferences.getUShort("port", 1883);
  preferences.end();

  if (digitalRead(CONFIG_BUTTON_PIN) == LOW || !wifiSsid.length() || !mqttBroker.length()) {
    startConfigMode();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.printf("Conectando a Wi-Fi: %s\n", wifiSsid.c_str());
  const unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    checkConfigButton();
    if (configMode) return;
    delay(25);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nNo se pudo conectar al Wi-Fi; iniciando modo de configuracion.");
    startConfigMode();
    return;
  }

  Serial.printf("Wi-Fi conectado. IP: %s\n", WiFi.localIP().toString().c_str());
  mqtt.setServer(mqttBroker.c_str(), mqttPort);
  mqtt.setBufferSize(128);
}

void loop()
{
  checkConfigButton();
  if (configMode) {
    dnsServer.processNextRequest();
    configServer.handleClient();
    delay(2);
    return;
  }

  updateMotionState();

  connectWifiIfNeeded();
  connectMqttIfNeeded();
  mqtt.loop();
  publishMotionStateIfNeeded();
  delay(10);
}
