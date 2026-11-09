#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Preferences.h>

// Conecta OUT del sensor PIR a GPIO 26 y comparte GND con el ESP32.
const uint8_t PIR_PIN = 26;
// Botón de configuración entre GPIO 27 y GND.
const uint8_t CONFIG_BUTTON_PIN = 27;
const unsigned long CONFIG_HOLD_MS = 3000;

const char *CONFIG_AP_SSID = "Movimiento-Config";
const char *CONFIG_AP_PASSWORD = "movimiento123";
const char *MOTION_TOPIC = "sensor/movimiento";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer configServer(80);
Preferences preferences;

String wifiSsid;
String wifiPassword;
String mqttBroker;
uint16_t mqttPort = 1883;

bool configMode = false;
bool motionState = false;
bool statePublished = false;
bool pirHasChanged = false;
unsigned long lastMqttAttempt = 0;
unsigned long configButtonPressedAt = 0;

String configPage(const String &message = "")
{
  String page = F("<!doctype html><html lang='es'><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>Configurar sensor de movimiento</title>"
                  "<style>body{font:16px sans-serif;max-width:480px;margin:32px auto;padding:0 16px}"
                  "input{box-sizing:border-box;width:100%;padding:10px;margin:6px 0 14px}"
                  "button{padding:12px 18px}</style><h1>Sensor de movimiento</h1>");
  if (message.length()) page += "<p>" + message + "</p>";
  page += F("<form method='post' action='/save'>"
            "<label>Nombre del Wi-Fi</label><input name='ssid' required maxlength='32'>"
            "<label>Clave del Wi-Fi</label><input name='password' type='password' maxlength='64'>"
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

    preferences.begin("motioncfg", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.putString("broker", broker);
    preferences.putUShort("port", (uint16_t)port);
    preferences.end();

    configServer.send(200, "text/html; charset=utf-8",
      "<meta charset='utf-8'><h1>Configuracion guardada</h1><p>El ESP32 se reiniciara.</p>");
    delay(1000);
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

void connectMqttIfNeeded()
{
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttempt < 3000) return;
  lastMqttAttempt = millis();

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "ESP32-movimiento-%llX", ESP.getEfuseMac());
  if (mqtt.connect(clientId)) {
    Serial.println("MQTT conectado; publicando estado en sensor/movimiento");
    statePublished = false; // Publica el estado actual al reconectar.
  } else {
    Serial.printf("Error al conectar MQTT, estado=%d; se reintentara\n", mqtt.state());
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
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.printf("Conectando a Wi-Fi: %s\n", wifiSsid.c_str());
  const unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(500);
    Serial.print('.');
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
    configServer.handleClient();
    delay(2);
    return;
  }

  const bool currentMotion = digitalRead(PIR_PIN) == HIGH;
  if (currentMotion != motionState) {
    motionState = currentMotion;
    pirHasChanged = true;
  }

  connectMqttIfNeeded();
  mqtt.loop();
  publishMotionStateIfNeeded();
  delay(10);
}
