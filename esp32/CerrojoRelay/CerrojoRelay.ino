#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Preferences.h>

// Usa un GPIO libre de tu placa. GPIO 26 funciona en la mayoría de ESP32 DevKit.
const uint8_t RELAY_PIN = 26;
const bool RELAY_ACTIVE_LOW = true; // Muchos módulos relay se activan llevando IN a LOW.
const uint8_t CONFIG_BUTTON_PIN = 27; // Botón entre GPIO 27 y GND
const char *CONFIG_AP_SSID = "Cerrojo-Config";
const char *CONFIG_AP_PASSWORD = "cerrojo123"; // Mínimo 8 caracteres
const unsigned long CONFIG_HOLD_MS = 3000;

const char *COMMAND_TOPIC = "actuador/cerrojo";
const char *STATE_TOPIC = "sensor/cerrojo";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer configServer(80);
Preferences preferences;
String wifiSsid;
String wifiPassword;
String mqttBroker;
uint16_t mqttPort = 1883;
bool isClosed = false;
unsigned long lastMqttAttempt = 0;
bool configMode = false;
unsigned long configButtonPressedAt = 0;

void setRelay(bool energized)
{
  const uint8_t onLevel = RELAY_ACTIVE_LOW ? LOW : HIGH;
  const uint8_t offLevel = RELAY_ACTIVE_LOW ? HIGH : LOW;
  digitalWrite(RELAY_PIN, energized ? onLevel : offLevel);
}

void publishState()
{
  if (mqtt.connected()) {
    mqtt.publish(STATE_TOPIC, isClosed ? "ON" : "OFF", true);
  }
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
    isClosed = true;
    setRelay(true);
    publishState();
    Serial.println("Comando recibido: CERRAR");
  } else if (command == "ABRIR") {
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

String configPage(const String &message = "")
{
  String page = F("<!doctype html><html lang='es'><meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<meta charset='utf-8'><title>Configurar cerrojo</title>"
                  "<style>body{font:16px sans-serif;max-width:480px;margin:32px auto;padding:0 16px}"
                  "input{box-sizing:border-box;width:100%;padding:10px;margin:6px 0 14px}"
                  "button{padding:12px 18px}</style><h1>Configurar cerrojo</h1>");
  if (message.length()) page += "<p>" + message + "</p>";
  page += F("<form method='post' action='/save'>"
            "<label>Nombre de Wi-Fi</label><input name='ssid' required maxlength='32'>"
            "<label>Clave de Wi-Fi</label><input name='password' type='password' maxlength='64'>"
            "<label>IP o nombre del broker MQTT</label><input name='broker' required maxlength='64'>"
            "<label>Puerto MQTT</label><input name='port' type='number' min='1' max='65535' value='1883' required>"
            "<button type='submit'>Guardar y reiniciar</button></form></html>");
  return page;
}

void startConfigMode()
{
  if (configMode) return;
  configMode = true;
  mqtt.disconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASSWORD);

  configServer.on("/", HTTP_GET, []() {
    configServer.send(200, "text/html; charset=utf-8", configPage());
  });
  configServer.on("/save", HTTP_POST, []() {
    String ssid = configServer.arg("ssid");
    String password = configServer.arg("password");
    String broker = configServer.arg("broker");
    long port = configServer.arg("port").toInt();
    if (!ssid.length() || !broker.length() || port < 1 || port > 65535) {
      configServer.send(400, "text/html; charset=utf-8", configPage("Revisa los datos ingresados."));
      return;
    }
    preferences.begin("lockcfg", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putString("broker", broker);
    preferences.putUShort("port", (uint16_t)port);
    preferences.end();
    configServer.send(200, "text/html; charset=utf-8",
      "<meta charset='utf-8'><h1>Guardado</h1><p>El ESP32 se reiniciara para conectar con la nueva configuracion.</p>");
    delay(1200);
    ESP.restart();
  });
  configServer.begin();
  Serial.printf("Modo configuracion: conectate a %s (clave %s) y abre http://192.168.4.1\n",
                CONFIG_AP_SSID, CONFIG_AP_PASSWORD);
}

void checkConfigButton()
{
  if (digitalRead(CONFIG_BUTTON_PIN) == LOW) {
    if (configButtonPressedAt == 0) configButtonPressedAt = millis();
    if (millis() - configButtonPressedAt >= CONFIG_HOLD_MS) startConfigMode();
  } else {
    configButtonPressedAt = 0;
  }
}

void setup()
{
  Serial.begin(115200);

  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

  // Inicializa el relay apagado para evitar un accionamiento durante el arranque.
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);

  preferences.begin("lockcfg", true);
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
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.print("Conectando al WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(500);
    Serial.print('.');
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
  if (configMode) {
    configServer.handleClient();
    delay(2);
    return;
  }
  connectMqttIfNeeded();
  mqtt.loop();
  delay(2);
}
