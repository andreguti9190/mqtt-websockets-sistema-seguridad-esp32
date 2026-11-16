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

### Inicio rápido en Windows

En una computadora nueva, copia la carpeta completa y haz doble clic en `Instalar-Sistema.cmd`. El instalador solicita permisos de administrador, descarga Node.js LTS desde `nodejs.org` y Mosquitto desde `mosquitto.org` si faltan, instala las dependencias npm, habilita únicamente los puertos privados necesarios y crea un acceso directo en el escritorio. Requiere Windows de 64 bits y conexión a Internet durante esta primera instalación.

Después, haz doble clic en `Iniciar-Sistema.cmd` o en el acceso directo del escritorio. El iniciador comprueba los programas necesarios, inicia MQTT, el servidor y el Quick Tunnel, muestra las direcciones del panel y del broker, y abre automáticamente el enlace público en el navegador. Si el servicio de Mosquitto de Windows está ocupando el puerto 1883, aparecerá una solicitud UAC para reemplazarlo mientras el sistema esté abierto.

Mantén la ventana del iniciador abierta. `Ctrl+C` detiene los componentes que inició y cierra el túnel público.

### Inicio manual

1. Instala Mosquitto en el mismo equipo que ejecutará el servidor. El servidor Node se conecta localmente a `mqtt://localhost:1883`; cada ESP32 recibe desde su portal la IP o hostname LAN vigente y el puerto. No hay una IP de broker fija en el firmware.
2. Abre una terminal en la carpeta del proyecto y ejecuta:

   ```powershell
   npm.cmd install
   npm.cmd start
   ```

   Si Mosquitto no está configurado para aceptar equipos de la red local, este repositorio incluye `mosquitto.conf`. Detén primero cualquier servicio que ya esté ocupando el puerto 1883 y, en otra terminal, ejecuta:

   ```powershell
   npm.cmd run broker
   ```

   En Windows, permite TCP `1883` solamente en el perfil de red privada. La configuración incluida acepta clientes sin contraseña y no debe publicarse en Internet.

3. El panel web escucha por defecto en el puerto HTTP `80`. `npm start` lo deja disponible solamente en el equipo y la red local.
4. Abre la URL local que aparece en la consola. Si necesitas publicar el panel en Internet de forma consciente, usa `npm.cmd run start:tunnel`; la URL aleatoria `trycloudflare.com` aparecerá en la consola.
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
- El portal detecta las redes cercanas para seleccionarlas de una lista; permite volver a escanear y configurar en cualquier momento la IP o hostname y el puerto MQTT.
- Ingresa el Wi-Fi y la IP LAN del broker MQTT; el puerto predeterminado es `1883`.
- El ESP32 publica `ON` mientras la salida estable del PIR está en HIGH y `OFF` cuando vuelve a LOW, en `sensor/movimiento`. El filtro de 100 ms evita cambios espurios sin ocultar el temporizador real del módulo.
- El firmware publica `ONLINE`/`OFFLINE` de forma retenida en `sensor/movimiento/disponibilidad`; si el ESP se desconecta, el servidor limpia el estado activo del dashboard.

### Cerrojo con relay

En `esp32/CerrojoRelay/CerrojoRelay.ino`:

- La señal `IN` del módulo relay va a GPIO `26`; usa una alimentación adecuada para el módulo y el cerrojo. No alimentes el cerrojo desde el pin de 3,3 V del ESP32.
- El sketch asume un relay activo en LOW (`RELAY_ACTIVE_LOW = true`). Cambia ese valor si tu módulo se activa en HIGH.
- Conecta un botón externo entre GPIO `27` y GND. No es el botón `EN`/`RST` de la placa: ese botón reinicia el microcontrolador y el programa no puede leerlo. Mantén GPIO 27 presionado 15 segundos, incluso si empiezas a sostenerlo durante el arranque, para abrir la configuración.
- Conéctate a `Cerrojo-Config` (clave `cerrojo123`). El portal cautivo debería abrirse automáticamente; si no lo hace, visita `http://192.168.4.1`.
- `192.168.4.1` pertenece únicamente al portal temporal del ESP32; no es una dirección fija del broker.
- El portal escanea las redes cercanas: selecciona el Wi-Fi de la lista, escribe su contraseña y usa “Volver a buscar redes” si la red todavía no aparece.
- En “broker MQTT” ingresa la IP LAN o hostname vigente de la computadora que ejecuta Mosquitto y este servidor, junto con el puerto que corresponda. Los valores quedan guardados pero pueden cambiarse siempre volviendo a abrir `Cerrojo-Config`. `localhost` no sirve desde el ESP32 porque apuntaría al propio ESP32.
- El ESP32 escucha `CERRAR` y `ABRIR` en `actuador/cerrojo`, y publica el estado `ON`/`OFF` en `sensor/cerrojo`.

#### Lector RFID RC522

Instala en Arduino IDE la biblioteca **MFRC522 by GithubCommunity**. Conecta el lector al mismo ESP32 del cerrojo:

| RC522 | ESP32 |
| --- | --- |
| `SDA` / `SS` | GPIO `21` |
| `SCK` | GPIO `18` |
| `MOSI` | GPIO `23` |
| `MISO` | GPIO `19` |
| `RST` | GPIO `22` |
| `IRQ` | Sin conectar |
| `GND` | `GND` |
| `3.3V` | `3V3` |

No alimentes el RC522 con 5 V. El botón de GPIO `27` separa claramente las operaciones:

- Pulsar y soltar en menos de 2 segundos: modo alta durante 45 segundos. Las tarjetas nuevas se agregan; una ya autorizada se informa pero nunca se elimina accidentalmente.
- Mantener 7 segundos: modo baja durante 45 segundos. La tarjeta presentada se elimina de la memoria.
- Mantener 15 segundos: abre `Cerrojo-Config`. Una pulsación intermedia entre 2 y 7 segundos no realiza cambios.

El LED controlable del ESP32 indica el modo: pulso breve cada 2 segundos en funcionamiento normal, parpadeo de 700 ms sin conexión, parpadeo muy rápido en modo alta, doble destello en modo baja y parpadeo lento en configuración. Una lectura aceptada o rechazada produce además una secuencia corta de confirmación.

Fuera del modo de alta, una tarjeta autorizada abre durante 5 segundos y luego vuelve a cerrar. Las tarjetas quedan guardadas en la memoria del ESP32, funcionan sin Wi-Fi y admiten hasta 20 credenciales. El ESP publica los resultados en `evento/cerrojo/rfid`; el servidor sólo los muestra en consola y no agrega tablas nuevas.

Si el lector reconoce una tarjeta una sola vez y después deja de responder, carga temporalmente `esp32/DiagnosticoRC522/DiagnosticoRC522.ino`. El monitor serie imprime `VersionReg` cada 2 segundos y cada UID leído, sin Wi-Fi, MQTT ni relay. Un valor estable distinto de `0x00`/`0xFF` indica que SPI responde; `0x00` o `0xFF` apunta a alimentación, falso contacto o cableado SPI.

Comprueba que la salida del PIR y la entrada del relay sean compatibles con los GPIO de 3,3 V del ESP32. Si el sensor entrega 5 V en `OUT`, usa un conversor de nivel antes de conectarlo al ESP32.

### Cámara ESP32-CAM

El firmware existente tiene su propia configuración Wi-Fi y de servidor. Su página de configuración se abre conectándose al AP `Camara-BridgeArcson-Config` (clave `12345678`). Configura la IP LAN del servidor y el puerto UDP `3000`. La cámara debe alcanzar el servidor por la red local; el Quick Tunnel solo expone el panel web.

## Tópicos MQTT

| Tópico | Mensaje | Función |
| --- | --- | --- |
| `sensor/movimiento` | `ON` / `OFF` | Estado del PIR y actualización de la página |
| `actuador/cerrojo` | `CERRAR` / `ABRIR` | Comandos del panel al ESP32 del relay |
| `sensor/cerrojo` | `ON` / `OFF` | Estado del cerrojo informado por el ESP32 |
| `evento/cerrojo/rfid` | Evento y UID | Alta, baja, acceso autorizado o denegado por RFID |

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
