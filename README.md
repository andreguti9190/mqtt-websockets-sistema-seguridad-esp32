# Sistema de seguridad Bridge Arcson

Panel web para ver la cámara ESP32-CAM y consultar el sensor de movimiento, con control MQTT del cerrojo mediante un relay. El proyecto usa un servidor Node.js, un broker MQTT y tres sketches Arduino independientes.

## Componentes

- `server/server.js`: sirve la web, comunica el frontend con MQTT, recibe los frames de cámara por UDP y lanza un Cloudflare Quick Tunnel.
- `client/`: interfaz web.
- `esp32/CameraWebServerBridegeArcson/CameraWebServerBridegeArcson.ino`: firmware de la ESP32-CAM. Envía vídeo por UDP con fragmentación y recuperación FEC.
- `esp32/SensorMovimiento/SensorMovimiento.ino`: firmware para una ESP32 separada con sensor PIR.
- `esp32/CerrojoRelay/CerrojoRelay.ino`: firmware para otra ESP32 separada que controla el relay del cerrojo.

## Dependencias

### Servidor

- Node.js **22.13 o posterior** y npm.
- Mosquitto u otro broker MQTT compatible, escuchando en `localhost:1883` desde el equipo servidor.
- `cloudflared` instalado y disponible en el `PATH`. El servidor lo ejecuta por nombre para que el proyecto no dependa de una ruta específica de Windows.
- Red local entre el servidor, el broker y los ESP32.

Las dependencias de Node (`express`, `mqtt` y `socket.io`) se instalan con `npm install`. SQLite se importa desde `node:sqlite`, incluido en Node; no necesita instalar un servidor SQLite aparte. La base local se guarda en `data/security.sqlite`.

### ESP32

- Arduino IDE o Arduino CLI con el paquete de placas **esp32 by Espressif Systems**.
- Biblioteca Arduino **PubSubClient** para `SensorMovimiento.ino` y `CerrojoRelay.ino`.
- Para la cámara AI Thinker, selecciona una placa ESP32-CAM compatible con PSRAM y una partición con espacio suficiente para el firmware.

## Puesta en marcha

1. Instala y configura el broker MQTT en el mismo equipo que ejecutará el servidor. El servidor Node se conecta a `mqtt://localhost:1883`.
2. Abre una terminal en la carpeta del proyecto y ejecuta:

   ```powershell
   npm install
   npm start
   ```

3. El panel web escucha por defecto en el puerto HTTP `80`. El Quick Tunnel se inicia automáticamente y su URL aleatoria `trycloudflare.com` aparece en la consola. Mantén ejecutándose el servidor y `cloudflared` para que el enlace funcione.
4. Abre la URL local o la URL del Quick Tunnel en el navegador.
5. Carga cada sketch en su ESP32. La cámara, el sensor PIR y el relay van en placas separadas.

Si el puerto 80 está ocupado o requiere permisos, configura otro puerto antes de iniciar. Por ejemplo, en PowerShell:

```powershell
$env:HTTP_PORT="8080"
npm start
```

El túnel usará ese puerto automáticamente. El servidor también mantiene un listener Socket.IO local en `2000`; la recepción UDP de la cámara usa `3000` por defecto (`UDP_PORT`).

## Configuración de los ESP32

### Sensor PIR

En `esp32/SensorMovimiento/SensorMovimiento.ino`:

- Conecta `OUT` del PIR a GPIO `26` y GND a GND común.
- Conecta un botón entre GPIO `27` y GND para abrir el modo de configuración.
- Mantén el botón presionado 3 segundos, conéctate a `Movimiento-Config` (clave `movimiento123`) y abre `http://192.168.4.1`.
- Ingresa el Wi-Fi y la IP LAN del broker MQTT; el puerto predeterminado es `1883`.
- El ESP32 publica `ON` mientras detecta movimiento y `OFF` cuando no lo detecta, en `sensor/movimiento`.

### Cerrojo con relay

En `esp32/CerrojoRelay/CerrojoRelay.ino`:

- La señal `IN` del módulo relay va a GPIO `26`; usa una alimentación adecuada para el módulo y el cerrojo. No alimentes el cerrojo desde el pin de 3,3 V del ESP32.
- El sketch asume un relay activo en LOW (`RELAY_ACTIVE_LOW = true`). Cambia ese valor si tu módulo se activa en HIGH.
- Conecta un botón entre GPIO `27` y GND. Manténlo presionado 3 segundos para configurar Wi-Fi y MQTT mediante `Cerrojo-Config` (clave `cerrojo123`) y `http://192.168.4.1`.
- El ESP32 escucha `CERRAR` y `ABRIR` en `actuador/cerrojo`, y publica el estado `ON`/`OFF` en `sensor/cerrojo`.

Comprueba que la salida del PIR y la entrada del relay sean compatibles con los GPIO de 3,3 V del ESP32. Si el sensor entrega 5 V en `OUT`, usa un conversor de nivel antes de conectarlo al ESP32.

### Cámara ESP32-CAM

El firmware existente tiene su propia configuración Wi-Fi y de servidor. Su página de configuración se abre conectándose al AP `Camara-BridgeArcson-Config` (clave `12345678`). Configura la IP LAN del servidor y el puerto UDP `3000`. La cámara debe alcanzar el servidor por la red local; el Quick Tunnel solo expone el panel web.

## Tópicos MQTT

| Tópico | Mensaje | Función |
| --- | --- | --- |
| `sensor/movimiento` | `ON` / `OFF` | Estado del PIR y actualización de la página |
| `actuador/cerrojo` | `CERRAR` / `ABRIR` | Comandos del panel al ESP32 del relay |
| `sensor/cerrojo` | `ON` / `OFF` | Estado del cerrojo informado por el ESP32 |

## Puertos

| Puerto | Protocolo | Uso |
| --- | --- | --- |
| `80` por defecto (`HTTP_PORT`) | TCP | Panel web y Socket.IO; destino del Quick Tunnel |
| `2000` | TCP | Listener local adicional de Express/Socket.IO |
| `3000` por defecto (`UDP_PORT`) | UDP | Frames enviados por la ESP32-CAM |
| `1883` | TCP | Broker MQTT por defecto |

Permite en el firewall local el tráfico que necesites entre los ESP32 y el equipo servidor, en particular MQTT y UDP para la cámara.

## Logs y apagado

Los mensajes del servidor se colorean en terminal: la IP local aparece en azul y la URL del Quick Tunnel en naranja. El resumen de cámara muestra FPS, frames perdidos y frames recuperados por FEC cada 5 segundos. Usa `Ctrl+C` para cerrar HTTP/Socket.IO, MQTT, UDP y el túnel.

## Acceso remoto

El Quick Tunnel es temporal y crea una URL pública nueva al reiniciar. No requiere correo ni autenticación de Cloudflare; cualquier persona que tenga la URL puede abrir el panel y accionar el cerrojo. Comparte el enlace únicamente con personas de confianza y detén el servidor para cerrar el acceso.
