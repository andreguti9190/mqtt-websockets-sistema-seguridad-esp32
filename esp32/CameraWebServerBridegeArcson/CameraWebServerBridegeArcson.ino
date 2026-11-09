#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <errno.h>
#include "esp_heap_caps.h"

#include "board_config.h"

// ======================= PINES =======================
#define BUTTON_PIN 13
#define HOLD_TIME  5000

#define LED_PIN  33      // LED rojo integrado (activo en LOW)
#define LED_ON   LOW
#define LED_OFF  HIGH
#define LED_CONFIG_BLINK_MS 200

// ======================= ENVIO UDP + FEC =======================
const size_t        UDP_CHUNK_SIZE    = 1200;
const uint8_t       UDP_MAX_RETRIES   = 3;
const uint16_t      CHUNK_DELAY_US    = 1000;
const unsigned long FRAME_INTERVAL_MS = 100;

#define FEC_PARITY_SHARDS    5
#define FEC_MAX_DATA_SHARDS  96

struct __attribute__((packed)) FECPacketHeader {
    uint32_t frameId;
    uint16_t chunkIndex;
    uint16_t kTotal;
    uint16_t mTotal;
    uint16_t chunkSize;
    uint16_t payloadSize;
    uint32_t totalSize;
    uint32_t crc32;
};
static_assert(sizeof(FECPacketHeader) == 22, "Unexpected UDP packet header size");

// ======================= ESTADO GLOBAL =======================
WebServer   webServer(80);
Preferences preferences;
WiFiUDP     udp;

unsigned long buttonPressStart = 0;
bool buttonPressed = false;
bool configMode    = false;

String wifiSSID     = "";
String wifiPassword = "";
String localIP      = "192.168.2.43";
String gatewayIP    = "192.168.2.1";
String subnetIP     = "255.255.255.0";
String dns1IP       = "8.8.8.8";
String dns2IP       = "1.1.1.1";
String serverIP     = "192.168.2.23";
uint16_t serverPort = 3000;

uint32_t frameId       = 0;
uint32_t framesWithinFecBudget = 0;
uint32_t framesAborted = 0;
uint32_t udpSentDatagrams = 0;
uint32_t sendRetryAttempts = 0;
uint32_t sendFailedChunks = 0;
int      lastSendErrno = 0;
const char *lastSendStage = "none";

uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

// ======================= ARITMETICA GF(256) =======================
static uint8_t GF_EXP[512];
static uint8_t GF_LOG[256];

void gfInitTables()
{
    uint16_t x = 1;

    for (int i = 0; i < 255; i++) {
        GF_EXP[i] = (uint8_t)x;
        GF_LOG[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++) {
        GF_EXP[i] = GF_EXP[i - 255];
    }
}

inline uint8_t gfMul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    return GF_EXP[GF_LOG[a] + GF_LOG[b]];
}

inline uint8_t gfDiv(uint8_t a, uint8_t b)
{
    if (a == 0) return 0;

    int diff = (int)GF_LOG[a] - (int)GF_LOG[b];
    if (diff < 0) diff += 255;
    return GF_EXP[diff];
}

uint8_t gfPow(uint8_t a, int power)
{
    if (power == 0) return 1;
    if (a == 0) return 0;

    int e = ((int)GF_LOG[a] * power) % 255;
    if (e < 0) e += 255;
    return GF_EXP[e];
}

// Inversion de matriz por Gauss-Jordan. 'm' se destruye, 'inv' recibe el resultado.
bool gfInvertMatrix(uint8_t *m, uint8_t *inv, int size)
{
    for (int i = 0; i < size; i++) {
        for (int j = 0; j < size; j++) {
            inv[i * size + j] = (i == j) ? 1 : 0;
        }
    }
    for (int col = 0; col < size; col++) {
        int pivotRow = -1;

        for (int row = col; row < size; row++) {
            if (m[row * size + col] != 0) {
                pivotRow = row;
                break;
            }
        }
        if (pivotRow == -1) return false;

        if (pivotRow != col) {
            for (int k = 0; k < size; k++) {
                uint8_t tmp = m[col * size + k];
                m[col * size + k]        = m[pivotRow * size + k];
                m[pivotRow * size + k]   = tmp;

                tmp = inv[col * size + k];
                inv[col * size + k]      = inv[pivotRow * size + k];
                inv[pivotRow * size + k] = tmp;
            }
        }

        uint8_t pivotInv = gfDiv(1, m[col * size + col]);
        for (int k = 0; k < size; k++) {
            m[col * size + k]   = gfMul(m[col * size + k], pivotInv);
            inv[col * size + k] = gfMul(inv[col * size + k], pivotInv);
        }

        for (int row = 0; row < size; row++) {
            if (row == col) continue;

            uint8_t factor = m[row * size + col];
            if (factor == 0) continue;

            for (int k = 0; k < size; k++) {
                m[row * size + k]   ^= gfMul(factor, m[col * size + k]);
                inv[row * size + k] ^= gfMul(factor, inv[col * size + k]);
            }
        }
    }
    return true;
}

// Filas de paridad de la matriz generadora sistematica (Vandermonde).
bool buildParityMatrix(int K, int M, uint8_t *parityMatrix)
{
    static uint8_t A[FEC_MAX_DATA_SHARDS * FEC_MAX_DATA_SHARDS];
    static uint8_t Ainv[FEC_MAX_DATA_SHARDS * FEC_MAX_DATA_SHARDS];

    for (int i = 0; i < K; i++) {
        for (int j = 0; j < K; j++) {
            A[i * K + j] = gfPow((uint8_t)(i + 1), j);
        }
    }

    if (!gfInvertMatrix(A, Ainv, K)) return false;

    for (int p = 0; p < M; p++) {
        uint8_t x = (uint8_t)(K + p + 1);

        for (int j = 0; j < K; j++) {
            uint8_t acc = 0;
            for (int jp = 0; jp < K; jp++) {
                acc ^= gfMul(gfPow(x, jp), Ainv[jp * K + j]);
            }
            parityMatrix[p * K + j] = acc;
        }
    }
    return true;
}

void computeParityShards(uint8_t **dataShards, int K, int M, int chunkSize,
                         uint8_t **parityShards, const uint8_t *parityMatrix)
{
    for (int p = 0; p < M; p++) {
        uint8_t *dst = parityShards[p];
        memset(dst, 0, chunkSize);

        for (int j = 0; j < K; j++) {
            uint8_t coef = parityMatrix[p * K + j];
            if (coef == 0) continue;

            const uint8_t *src = dataShards[j];
            for (int b = 0; b < chunkSize; b++) {
                dst[b] ^= gfMul(coef, src[b]);
            }
        }
    }
}

// ======================= ENVIO =======================
bool sendChunk(const IPAddress &addr, uint16_t port,
               const FECPacketHeader &header,
               const uint8_t *data, size_t len)
{
    for (uint8_t attempt = 0; attempt < UDP_MAX_RETRIES; attempt++) {
        errno = 0;
        if (!udp.beginPacket(addr, port)) {
            lastSendStage = "beginPacket";
            lastSendErrno = errno;
            sendRetryAttempts++;
            delay(8);
            continue;
        }

        const size_t headerWritten = udp.write((const uint8_t *)&header, sizeof(header));
        const size_t payloadWritten = udp.write(data, len);
        errno = 0;
        const bool packetSent = udp.endPacket();

        if (headerWritten == sizeof(header) && payloadWritten == len && packetSent) {
            udpSentDatagrams++;
            return true;
        }

        lastSendStage = (headerWritten != sizeof(header) || payloadWritten != len)
            ? "udp.write" : "endPacket/sendto";
        lastSendErrno = errno;
        sendRetryAttempts++;
        delay(8);   // dejar que se liberen los buffers de la pila Wi-Fi
    }
    return false;
}

bool sendFrameFEC(camera_fb_t *fb)
{
    static IPAddress serverAddr;
    static String    cachedServer = "";

    static uint8_t  lastShardBuf[UDP_CHUNK_SIZE];
    static uint8_t *dataShards[FEC_MAX_DATA_SHARDS];
    static uint8_t  parityMatrix[FEC_PARITY_SHARDS * FEC_MAX_DATA_SHARDS];
    static uint8_t  parityBuf[FEC_PARITY_SHARDS][UDP_CHUNK_SIZE];
    static uint8_t *parityShards[FEC_PARITY_SHARDS];
    static int      matrixK = 0;

    if (cachedServer != serverIP) {
        if (!serverAddr.fromString(serverIP)) {
            Serial.println("ERROR: IP del servidor invalida.");
            return false;
        }
        cachedServer = serverIP;
    }

    int K = (fb->len + UDP_CHUNK_SIZE - 1) / UDP_CHUNK_SIZE;
    if (K < 1) K = 1;

    if (K > FEC_MAX_DATA_SHARDS) {
        Serial.printf("Frame demasiado grande (K=%d > %d), se descarta.\n", K, FEC_MAX_DATA_SHARDS);
        framesAborted++;
        frameId++;
        return false;
    }

    const int M = FEC_PARITY_SHARDS;

    // La matriz de paridad solo depende de K y M: se recalcula unicamente si K cambia.
    if (K != matrixK) {
        if (!buildParityMatrix(K, M, parityMatrix)) {
            Serial.println("ERROR: no se pudo construir la matriz FEC.");
            matrixK = 0;
            framesAborted++;
            frameId++;
            return false;
        }
        matrixK = K;
    }

    for (int i = 0; i < K; i++) {
        size_t offset = (size_t)i * UDP_CHUNK_SIZE;
        size_t len    = min((size_t)UDP_CHUNK_SIZE, (size_t)(fb->len - offset));

        if (len < UDP_CHUNK_SIZE) {
            memset(lastShardBuf, 0, UDP_CHUNK_SIZE);
            memcpy(lastShardBuf, fb->buf + offset, len);
            dataShards[i] = lastShardBuf;
        } else {
            dataShards[i] = fb->buf + offset;
        }
    }

    for (int p = 0; p < M; p++) parityShards[p] = parityBuf[p];

    computeParityShards(dataShards, K, M, UDP_CHUNK_SIZE, parityShards, parityMatrix);

    const int totalShards  = K + M;
    int       failedChunks = 0;

    for (int i = 0; i < totalShards; i++) {
        FECPacketHeader header;
        header.frameId    = frameId;
        header.chunkIndex = (uint16_t)i;
        header.kTotal     = (uint16_t)K;
        header.mTotal     = (uint16_t)M;
        header.chunkSize  = (uint16_t)UDP_CHUNK_SIZE;
        header.totalSize  = (uint32_t)fb->len;

        const uint8_t *payload = (i < K) ? dataShards[i] : parityShards[i - K];
        size_t payloadSize = UDP_CHUNK_SIZE;
        if (i == K - 1) {
            payloadSize = fb->len - ((size_t)(K - 1) * UDP_CHUNK_SIZE);
        }
        header.payloadSize = (uint16_t)payloadSize;
        header.crc32 = crc32(payload, payloadSize);

        if (!sendChunk(serverAddr, serverPort, header, payload, payloadSize)) {
            failedChunks++;
            sendFailedChunks++;

            // Solo se descarta el frame si se pierden mas paquetes de los que el FEC recupera.
            if (failedChunks > M) {
                framesAborted++;
                frameId++;
                return false;
            }
        }

        delayMicroseconds(CHUNK_DELAY_US);
    }

    frameId++;
    framesWithinFecBudget++;
    return true;
}

// ======================= CONFIGURACION GUARDADA =======================
bool loadConfiguration()
{
    if (!preferences.isKey("ssid")) {
        Serial.println("No existe configuracion WiFi.");
        return false;
    }

    wifiSSID     = preferences.getString("ssid", "");
    wifiPassword = preferences.getString("password", "");
    localIP      = preferences.getString("localIP", localIP);
    gatewayIP    = preferences.getString("gateway", gatewayIP);
    subnetIP     = preferences.getString("subnet", subnetIP);
    dns1IP       = preferences.getString("dns1", dns1IP);
    dns2IP       = preferences.getString("dns2", dns2IP);
    serverIP     = preferences.getString("server", serverIP);
    serverPort   = preferences.getUShort("port", serverPort);

    Serial.println();
    Serial.println("CONFIGURACION CARGADA");
    Serial.printf("SSID: %s\n", wifiSSID.c_str());
    Serial.printf("IP: %s\n", localIP.c_str());
    Serial.printf("Gateway: %s\n", gatewayIP.c_str());
    Serial.printf("Servidor: %s:%u\n", serverIP.c_str(), (unsigned)serverPort);

    return wifiSSID.length() > 0;
}

void saveConfiguration()
{
    preferences.putString("ssid",     wifiSSID);
    preferences.putString("password", wifiPassword);
    preferences.putString("localIP",  localIP);
    preferences.putString("gateway",  gatewayIP);
    preferences.putString("subnet",   subnetIP);
    preferences.putString("dns1",     dns1IP);
    preferences.putString("dns2",     dns2IP);
    preferences.putString("server",   serverIP);
    preferences.putUShort("port",     serverPort);
}

// ======================= WIFI =======================
bool connectWiFi()
{
    if (wifiSSID.length() == 0) {
        Serial.println("No hay SSID configurado.");
        return false;
    }

    Serial.println();
    Serial.printf("CONECTANDO A WIFI: %s\n", wifiSSID.c_str());

    IPAddress ip, gateway, subnet, dns1, dns2;

    if (!ip.fromString(localIP)       ||
        !gateway.fromString(gatewayIP) ||
        !subnet.fromString(subnetIP)   ||
        !dns1.fromString(dns1IP)       ||
        !dns2.fromString(dns2IP))
    {
        Serial.println("ERROR: IP, gateway, mascara o DNS invalidos.");
        return false;
    }

    WiFi.mode(WIFI_STA);
    delay(200);

    if (!WiFi.config(ip, gateway, subnet, dns1, dns2)) {
        Serial.println("ERROR configurando IP.");
    }

    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

    Serial.print("Conectando");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(300);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("NO SE PUDO CONECTAR.");
        WiFi.disconnect(true);
        return false;
    }

    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    Serial.println("WIFI CONECTADO");
    Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
    Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    Serial.printf("Servidor: %s:%u\n", serverIP.c_str(), (unsigned)serverPort);

    return true;
}

void scanNetworks()
{
    Serial.println();
    Serial.println("ESCANEANDO REDES WIFI");

    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false, true);

    Serial.printf("Redes encontradas: %d\n", n);

    for (int i = 0; i < n; i++) {
        Serial.printf("%d - %s | RSSI: %d | Canal: %d\n",
                      i, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
    }
}

// ======================= PAGINA DE CONFIGURACION =======================
const char PAGE_STYLE[] PROGMEM = R"rawliteral(
*{box-sizing:border-box}
html,body{margin:0;padding:0;font-family:Arial,sans-serif}
body{background:#062B5B;color:#FFF;min-height:100vh;padding:25px 15px}

.container{
  max-width:500px;margin:30px auto;padding:28px;
  background:#0B1D31;border:1px solid #29445F;border-radius:16px;
  box-shadow:0 10px 35px rgba(0,0,0,.35)
}

h1{text-align:center;margin:0 0 25px;font-size:28px;font-weight:700;letter-spacing:.5px}

label{display:block;margin:16px 0 6px;font-size:14px;font-weight:bold}

input,select{
  width:100%;margin-top:3px;padding:12px 14px;
  background:#14283D;color:#FFF;border:1px solid #344C63;border-radius:8px;
  font-size:15px;outline:none;transition:.2s
}
input:focus,select:focus{border-color:#9FE8F5;box-shadow:0 0 0 2px rgba(159,232,245,.15)}
input::placeholder{color:#8293A5}

button{
  width:100%;margin-top:20px;padding:13px;
  background:#A9EAF5;color:#062B5B;border:0;border-radius:8px;
  font-size:16px;font-weight:bold;cursor:pointer;transition:.2s
}
button:hover{background:#C8F3FA;transform:translateY(-1px);box-shadow:0 5px 15px rgba(159,232,245,.2)}

.scan{background:#163652;color:#A9EAF5;border:1px solid #345875}
.scan:hover{background:#1D4566;color:#FFF}

.info{
  margin-bottom:18px;padding:14px;
  background:#12263B;border:1px solid #29445F;border-radius:9px;
  color:#DCE7EF;font-size:14px
}

@media(max-width:600px){
  body{padding:10px}
  .container{margin:15px auto;padding:20px;border-radius:13px}
  h1{font-size:24px}
}
)rawliteral";

String htmlEscape(const String &s)
{
    String out;
    out.reserve(s.length() + 8);

    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;
        }
    }
    return out;
}

void addField(String &html, const char *label, const char *type,
              const char *name, const String &value)
{
    html += "<label>";
    html += label;
    html += "</label><input type='";
    html += type;
    html += "' name='";
    html += name;
    html += "' value=\"";
    html += htmlEscape(value);
    html += "\" required>";
}

String getConfigPage()
{
    String html;
    html.reserve(6144);

    html += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>CAMARA WIFI BRIDGE ARCSON</title><style>";
    html += PAGE_STYLE;
    html += "</style></head><body><div class='container'>";
    html += "<h1>CAMARA WIFI BRIDGE ARCSON</h1>";
    html += "<div class='info'>Selecciona una red detectada y completa la configuración.</div>";

    html += "<form method='POST' action='/save'>";
    html += "<label>Red WiFi</label><select name='ssid' required>";

    int n = WiFi.scanComplete();
    bool anyNetwork = false;

    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;

        String safe = htmlEscape(ssid);
        html += "<option value=\"" + safe + "\">" + safe;
        html += " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
        anyNetwork = true;
    }
    if (!anyNetwork) {
        html += "<option value=''>No se detectaron redes</option>";
    }
    html += "</select>";

    html += "<label>Contraseña WiFi</label>";
    html += "<input type='password' name='password' placeholder='Contraseña'>";

    addField(html, "IP del CAMARA WIFI",           "text",   "ip",      localIP);
    addField(html, "Gateway",                      "text",   "gateway", gatewayIP);
    addField(html, "Máscara de red",               "text",   "subnet",  subnetIP);
    addField(html, "DNS principal",                "text",   "dns1",    dns1IP);
    addField(html, "DNS secundario",               "text",   "dns2",    dns2IP);
    addField(html, "IP del servidor Bridge Arcson", "text",  "server",  serverIP);
    addField(html, "Puerto del servidor (Default 3000)", "number", "port", String(serverPort));

    html += "<button type='submit'>GUARDAR Y CONECTAR</button></form>";

    html += "<form method='GET' action='/scan'>";
    html += "<button class='scan' type='submit'>ESCANEAR REDES NUEVAMENTE</button></form>";

    html += "</div></body></html>";

    return html;
}

// ======================= HANDLERS WEB =======================
void handleRoot()
{
    webServer.send(200, "text/html", getConfigPage());
}

void handleScan()
{
    scanNetworks();
    webServer.sendHeader("Location", "/");
    webServer.send(303);
}

void handleSave()
{
    String newSSID    = webServer.arg("ssid");
    String newIP      = webServer.arg("ip");
    String newGateway = webServer.arg("gateway");
    String newSubnet  = webServer.arg("subnet");
    String newServer  = webServer.arg("server");

    if (newSSID.length() == 0 || newIP.length() == 0 || newGateway.length() == 0 ||
        newSubnet.length() == 0 || newServer.length() == 0)
    {
        webServer.send(400, "text/html", "<h2>Datos incompletos</h2>");
        return;
    }

    wifiSSID     = newSSID;
    wifiPassword = webServer.arg("password");
    localIP      = newIP;
    gatewayIP    = newGateway;
    subnetIP     = newSubnet;
    dns1IP       = webServer.arg("dns1");
    dns2IP       = webServer.arg("dns2");
    serverIP     = newServer;

    int port = webServer.arg("port").toInt();
    if (port > 0 && port <= 65535) {
        serverPort = port;
    }

    saveConfiguration();

    Serial.println();
    Serial.println("CONFIGURACION GUARDADA");
    Serial.printf("SSID: %s | IP: %s | Servidor: %s:%u\n",
                  wifiSSID.c_str(), localIP.c_str(), serverIP.c_str(), (unsigned)serverPort);

    String response;
    response += "<!DOCTYPE html><html><head><meta charset='UTF-8'></head>";
    response += "<body style='font-family:Arial;text-align:center;padding:30px'>";
    response += "<h2>Configuración guardada</h2>";
    response += "<p>El ESP32-CAM se conectará a:</p><b>" + htmlEscape(wifiSSID) + "</b>";
    response += "<p>Reiniciando conexión...</p></body></html>";

    webServer.send(200, "text/html", response);

    delay(1000);
    webServer.stop();
    WiFi.softAPdisconnect(true);
    delay(500);

    configMode = false;

    WiFi.disconnect(true);
    delay(500);

    if (!connectWiFi()) {
        Serial.println();
        Serial.println("Falló la conexión.");
        Serial.println("Mantén el botón 5 segundos para reconfigurar.");
    }
}

// ======================= MODO CONFIGURACION =======================
void startConfigMode()
{
    static bool routesRegistered = false;

    if (configMode) return;
    configMode = true;

    digitalWrite(LED_PIN, LED_ON);

    Serial.println();
    Serial.println("ENTRANDO A CONFIGURACION");

    udp.stop();
    WiFi.disconnect(false);
    delay(300);

    WiFi.mode(WIFI_AP_STA);
    delay(300);

    WiFi.softAPdisconnect(true);
    delay(300);

    if (!WiFi.softAP("Camara-BridgeArcson-Config", "12345678")) {
        Serial.println("ERROR creando Access Point.");
        configMode = false;
        return;
    }

    Serial.println("Access Point: Camara-BridgeArcson-Config (clave 12345678)");
    Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());

    scanNetworks();

    if (!routesRegistered) {
        webServer.on("/",     HTTP_GET,  handleRoot);
        webServer.on("/scan", HTTP_GET,  handleScan);
        webServer.on("/save", HTTP_POST, handleSave);
        routesRegistered = true;
    }

    webServer.begin();

    Serial.println("Servidor HTTP iniciado.");
    Serial.println("1. Conectate a Camara-BridgeArcson-Config");
    Serial.println("2. Clave: 12345678");
    Serial.println("3. Abre http://192.168.4.1");
}

// ======================= BOTON =======================
void checkButton()
{
    bool pressed = (digitalRead(BUTTON_PIN) == LOW);

    if (!pressed) {
        buttonPressed = false;
        return;
    }

    if (!buttonPressed) {
        buttonPressed = true;
        buttonPressStart = millis();
        Serial.println("Boton presionado...");
    }

    if (!configMode && millis() - buttonPressStart >= HOLD_TIME) {
        Serial.println();
        Serial.println("BOTON MANTENIDO 5 SEGUNDOS");
        startConfigMode();
    }
}

// ======================= CAMARA =======================
bool initCamera()
{
    camera_config_t config;

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_HD; // OV2640: 1280x720
    config.jpeg_quality = 20;           // OV: numero mayor = mas compresion y menor caudal
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    if (psramFound()) {
        config.fb_location = CAMERA_FB_IN_PSRAM;
    } else {
        // Sin PSRAM no hay memoria suficiente para sostener HD de forma fiable.
        config.frame_size   = FRAMESIZE_VGA;
        config.jpeg_quality = 20;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.fb_count    = 1;
        Serial.println("PSRAM no detectada: usando VGA para evitar fallos de captura.");
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Error camara: 0x%x\n", err);
        return false;
    }

    // Mantener salida JPEG, aplicando escala de grises en el sensor antes de transmitir.
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor && sensor->set_special_effect) {
        const int effectResult = sensor->set_special_effect(sensor, 2); // 2 = grayscale
        if (effectResult != 0) {
            Serial.printf("Aviso: no se pudo activar escala de grises (codigo %d).\n", effectResult);
        }
    }

    Serial.println("Camara iniciada.");
    return true;
}

// ======================= LUCES =======================
// LED rojo: fijo (parpadea en modo configuracion).
void updateLed()
{
    static unsigned long lastToggle = 0;
    static bool state = false;

    if (configMode) {
        if (millis() - lastToggle >= LED_CONFIG_BLINK_MS) {
            lastToggle = millis();
            state = !state;
            digitalWrite(LED_PIN, state ? LED_ON : LED_OFF);
        }
    } else {
        digitalWrite(LED_PIN, LED_ON);
    }
}

// ======================= SETUP / LOOP =======================
void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("ESP32-CAM INICIANDO");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_ON);

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    preferences.begin("wifi", false);
    gfInitTables();
    initCamera();

    if (!loadConfiguration()) {
        Serial.println("No hay WiFi configurado.");
        startConfigMode();
        return;
    }

    if (!connectWiFi()) {
        Serial.println("No se pudo conectar. Entrando en configuracion...");
        startConfigMode();
    }
}

void loop()
{
    updateLed();

    if (configMode) {
        webServer.handleClient();
        delay(1);
        return;
    }

    checkButton();

    if (WiFi.status() != WL_CONNECTED) {
        delay(100);
        return;
    }

    static unsigned long lastFrameMs = 0;
    static unsigned long lastStatMs  = 0;

    if (millis() - lastFrameMs < FRAME_INTERVAL_MS) {
        delay(1);
        return;
    }
    lastFrameMs = millis();

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Error capturando frame.");
        delay(10);
        return;
    }

    sendFrameFEC(fb);
    esp_camera_fb_return(fb);
    if (millis() - lastStatMs >= 5000) {
        lastStatMs = millis();
        Serial.printf("Frames dentro del limite FEC: %u | abortados: %u | datagramas sendto OK: %u | reintentos fallidos: %u | fragmentos no enviados: %u | RSSI: %d dBm | ultima falla: %s errno=%d | heap libre: %u (min %u) | heap interno libre/mayor bloque: %u/%u\n",
                      (unsigned)framesWithinFecBudget, (unsigned)framesAborted,
                      (unsigned)udpSentDatagrams, (unsigned)sendRetryAttempts, (unsigned)sendFailedChunks,
                      WiFi.RSSI(), lastSendStage, lastSendErrno,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        framesWithinFecBudget = 0;
        framesAborted = 0;
        udpSentDatagrams = 0;
        sendRetryAttempts = 0;
        sendFailedChunks = 0;
        lastSendErrno = 0;
        lastSendStage = "none";
    }
}
