import express from 'express'
import { Server } from 'socket.io'
import { createServer } from 'http'
import { existsSync, mkdirSync } from 'fs'
import { resolve, dirname } from 'path'
import { fileURLToPath } from 'url'
import mqtt from 'mqtt'
import { DatabaseSync } from 'node:sqlite'
import dgram from 'dgram'
import { spawn } from 'child_process'
import { networkInterfaces } from 'os'

const LOG_COLORS = { cámara: 36, mqtt: 35, web: 34, websocket: 94, sensor: 33, cerrojo: 32, cloudflare: '38;5;208' }
function log(tag, message, level = 'log') {
    const color = LOG_COLORS[tag.toLowerCase()] || 37
    const useColor = process.stdout.isTTY && !process.env.NO_COLOR
    const messageColor = level === 'error' ? 31 : level === 'warn' ? 33 : color
    const label = useColor ? `\x1b[${messageColor}m[${tag}]\x1b[0m` : `[${tag}]`
    const coloredMessage = useColor ? `\x1b[${messageColor}m${message}\x1b[0m` : message
    console[level](`${label} ${coloredMessage}`)
}

process.on('warning', (warning) => {
    // SQLite's bundled Node API is experimental in this Node release; keep the
    // startup console clean while still surfacing any unrelated warnings.
    if (warning.name === 'ExperimentalWarning' && /sqlite/i.test(warning.message)) return
    log('Node', warning.stack || warning.message, 'warn')
})

const port = 2000
const app = express()
const server = createServer(app)
const here = dirname(fileURLToPath(import.meta.url))
const clientDirectory = resolve(here, '../client')
app.use(express.static(clientDirectory))

const dataDirectory = resolve(here, '../data')
mkdirSync(dataDirectory, { recursive: true })
const db = new DatabaseSync(resolve(dataDirectory, 'security.sqlite'))
db.exec(`
    CREATE TABLE IF NOT EXISTS sensores (
        nombre TEXT PRIMARY KEY,
        estado INTEGER NOT NULL DEFAULT 0 CHECK (estado IN (0, 1))
    );
    INSERT OR IGNORE INTO sensores (nombre, estado) VALUES ('movimiento', 0);
    INSERT OR IGNORE INTO sensores (nombre, estado) VALUES ('cerrojo', 0);
`)
const updateSensorState = db.prepare('UPDATE sensores SET estado = ? WHERE nombre = ?')
// mqtt
const clientmqtt = mqtt.connect("mqtt://localhost:1883");
// websocket
const io = new Server(server, {
    cors: {
        origin: "*"
    }
})
const socketServers = [io]

let latestFrame = null;
let cameraClients = new Set();
let frameTimer = null;
let frameLostLogged = false;
const FRAME_TIMEOUT_MS = 5000;
const MAX_CLIENT_BUFFER = 512 * 1024;

function trackFrames() {
    clearTimeout(frameTimer);
    frameTimer = setTimeout(() => {
        if (frameLostLogged) return;
        log('Cámara', 'Se interrumpió la recepción de frames.');
        frameLostLogged = true;
    }, FRAME_TIMEOUT_MS);
}

function broadcastFrame(jpegBuffer) {

    latestFrame = jpegBuffer;

    if (frameLostLogged) {
        frameLostLogged = false;
        log('Cámara', 'Se restableció la recepción desde el ESP32.');
    }

    trackFrames();

    const header =
        `--frame\r\n` +
        `Content-Type: image/jpeg\r\n` +
        `Content-Length: ${jpegBuffer.length}\r\n\r\n`;

    for (const client of cameraClients) {

        // cliente desconectado
        if (client.destroyed || client.writableEnded) {
            cameraClients.delete(client);
            continue;
        }

        // cliente lento: no acumular frames en memoria, saltar este
        if (client.writableLength > MAX_CLIENT_BUFFER) {
            continue;
        }

        try {
            client.write(header);
            client.write(jpegBuffer);
            client.write('\r\n');
        } catch (error) {
            cameraClients.delete(client);
        }
    }
}

// ---------- FEC: aritmética en GF(256) (mismo polinomio 0x11D y generador 2 que el firmware) ----------
const GF_EXP = new Uint8Array(512);
const GF_LOG = new Uint8Array(256);

(function initGfTables() {
    let x = 1;
    for (let i = 0; i < 255; i++) {
        GF_EXP[i] = x;
        GF_LOG[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (let i = 255; i < 512; i++) GF_EXP[i] = GF_EXP[i - 255];
})();

function gfMul(a, b) {
    if (a === 0 || b === 0) return 0;
    return GF_EXP[GF_LOG[a] + GF_LOG[b]];
}

function gfDiv(a, b) {
    if (a === 0) return 0;
    let diff = GF_LOG[a] - GF_LOG[b];
    if (diff < 0) diff += 255;
    return GF_EXP[diff];
}

function gfPow(a, powerExp) {
    if (powerExp === 0) return 1;
    if (a === 0) return 0;
    let e = (GF_LOG[a] * powerExp) % 255;
    if (e < 0) e += 255;
    return GF_EXP[e];
}

function swapRows(arr, size, r1, r2) {
    for (let k = 0; k < size; k++) {
        const tmp = arr[r1 * size + k];
        arr[r1 * size + k] = arr[r2 * size + k];
        arr[r2 * size + k] = tmp;
    }
}

// Invierte una matriz size x size en GF(256) (Gauss-Jordan con pivoteo).
function gfInvertMatrix(input, size) {
    const m = Uint8Array.from(input);
    const inv = new Uint8Array(size * size);
    for (let i = 0; i < size; i++) inv[i * size + i] = 1;

    for (let col = 0; col < size; col++) {
        let pivotRow = -1;
        for (let row = col; row < size; row++) {
            if (m[row * size + col] !== 0) { pivotRow = row; break; }
        }
        if (pivotRow === -1) return null;

        if (pivotRow !== col) {
            swapRows(m, size, col, pivotRow);
            swapRows(inv, size, col, pivotRow);
        }

        const pivotInv = gfDiv(1, m[col * size + col]);
        for (let k = 0; k < size; k++) {
            m[col * size + k] = gfMul(m[col * size + k], pivotInv);
            inv[col * size + k] = gfMul(inv[col * size + k], pivotInv);
        }

        for (let row = 0; row < size; row++) {
            if (row === col) continue;
            const factor = m[row * size + col];
            if (factor === 0) continue;
            for (let k = 0; k < size; k++) {
                m[row * size + k] ^= gfMul(factor, m[col * size + k]);
                inv[row * size + k] ^= gfMul(factor, inv[col * size + k]);
            }
        }
    }
    return inv;
}

// Matriz generadora sistemática (K+M filas x K columnas): primeras K filas =
// identidad (paquetes de datos), últimas M = coeficientes de paridad.
// Debe coincidir exactamente con buildParityMatrix() del firmware ESP32.
function buildGeneratorMatrix(K, M) {
    const total = K + M;
    const G = new Uint8Array(total * K);

    for (let i = 0; i < K; i++) G[i * K + i] = 1;

    const A = new Uint8Array(K * K);
    for (let i = 0; i < K; i++)
        for (let j = 0; j < K; j++)
            A[i * K + j] = gfPow(i + 1, j);

    const Ainv = gfInvertMatrix(A, K);
    if (!Ainv) return null;

    for (let p = 0; p < M; p++) {
        const x = K + p + 1;
        for (let j = 0; j < K; j++) {
            let acc = 0;
            for (let jp = 0; jp < K; jp++) {
                acc ^= gfMul(gfPow(x, jp), Ainv[jp * K + j]);
            }
            G[(K + p) * K + j] = acc;
        }
    }
    return G;
}

// Reconstruye los K shards de datos a partir de al menos K shards recibidos
// (cualquier combinación de datos y/o paridad).
function reedSolomonDecode(shards, K, M, chunkSize) {
    const total = K + M;
    const G = buildGeneratorMatrix(K, M);
    if (!G) return null;

    const indices = [];
    for (let i = 0; i < total && indices.length < K; i++) {
        if (shards[i] !== null) indices.push(i);
    }
    if (indices.length < K) return null;

    const Gd = new Uint8Array(K * K);
    for (let r = 0; r < K; r++) {
        const srcRow = indices[r];
        for (let c = 0; c < K; c++) Gd[r * K + c] = G[srcRow * K + c];
    }

    const GdInv = gfInvertMatrix(Gd, K);
    if (!GdInv) return null;

    const dataShards = [];
    for (let j = 0; j < K; j++) dataShards.push(Buffer.alloc(chunkSize));

    for (let j = 0; j < K; j++) {
        const out = dataShards[j];
        for (let r = 0; r < K; r++) {
            const coef = GdInv[j * K + r];
            if (coef === 0) continue;
            const src = shards[indices[r]];
            for (let b = 0; b < chunkSize; b++) {
                out[b] ^= gfMul(coef, src[b]);
            }
        }
    }
    return dataShards;
}
// ---------- FIN FEC ----------

// ---------- UDP: recepción, FEC y reensamblado de frames del ESP32-CAM ----------
// Debe coincidir EXACTO con la struct FECPacketHeader del firmware:
// uint32_t frameId; uint16_t chunkIndex; uint16_t kTotal; uint16_t mTotal;
// uint16_t chunkSize; uint16_t payloadSize; uint32_t totalSize; uint32_t crc32;
// little-endian, packed = 22 bytes. CRC32 IEEE cubre el payload enviado.
//
// kTotal = paquetes de datos del frame, mTotal = paquetes de paridad (FEC).
// Con cualquier combinación de al menos kTotal de los (kTotal+mTotal)
// paquetes recibidos, el frame se puede reconstruir completo.
//
// IMPORTANTE: el ESP32 manda al "Puerto del servidor" configurado en su página
// (por defecto 3000). Este puerto UDP (3000) es distinto del puerto TCP de
// Express/Socket.IO (2000). Si en el ESP32 pones otro puerto, cambia esto
// o define la variable de entorno UDP_PORT.
const UDP_PORT = Number(process.env.UDP_PORT) || 3000;
const UDP_HEADER_SIZE = 4 + 2 + 2 + 2 + 2 + 2 + 4 + 4; // 22 bytes
const UDP_CHUNK_SIZE = 1200;        // debe coincidir con UDP_CHUNK_SIZE del firmware
const UDP_PARITY_SHARDS = 5;        // debe coincidir con FEC_PARITY_SHARDS
const UDP_MAX_DATA_SHARDS = 96;    // debe coincidir con FEC_MAX_DATA_SHARDS
const UDP_FRAME_TIMEOUT_MS = 300;    // descarta frames que nunca llegan a kTotal paquetes
const MAX_FRAME_SIZE = UDP_CHUNK_SIZE * UDP_MAX_DATA_SHARDS;
const MAX_SHARDS = UDP_MAX_DATA_SHARDS + UDP_PARITY_SHARDS;
const MAX_FRAMES_IN_PROGRESS = 4;   // limita memoria si llegan frames incompletos
const ESP_RESET_TIMEOUT_MS = 3000;   // si no llegan frames en este tiempo, se asume reinicio del ESP32

const framesInProgress = new Map();
let lastShownFrameId = null;
let lastPacketTime = 0;

// Contadores internos de recepción; el resumen periódico solo muestra métricas de frames.
const stats = {
    packetsReceived: 0,
    packetsAccepted: 0,
    invalidPackets: 0,
    crcErrors: 0,
    duplicatePackets: 0,
    framesOk: 0,
    framesRecovered: 0,
    dataShardsRecovered: 0,
    framesTimedOut: 0,
    framesEvicted: 0,
    decodeErrors: 0,
    invalidJpegs: 0,
    framesLost: 0,
    reassemblyMsTotal: 0,
    reassemblyMsMax: 0
};
let cameraStatsStartedAt = Date.now();
const cameraStatsInterval = setInterval(() => {
    const elapsedSeconds = (Date.now() - cameraStatsStartedAt) / 1000;
    const fps = elapsedSeconds > 0 ? (stats.framesOk / elapsedSeconds).toFixed(1) : '0.0';
    log('Cámara', `FPS: ${fps} | Frames perdidos: ${stats.framesLost} | Frames recuperados (FEC): ${stats.framesRecovered}`);
    cameraStatsStartedAt = Date.now();
    for (const key of Object.keys(stats)) stats[key] = 0;
}, 5000);

const udpServer = dgram.createSocket({ type: 'udp4', reuseAddr: true });

// buffer de recepción grande para no perder ráfagas de paquetes
udpServer.on('listening', () => {
    try {
        udpServer.setRecvBufferSize(4 * 1024 * 1024);
    } catch (e) {
        log('Cámara', `No se pudo ampliar el buffer UDP: ${e.message}`, 'warn');
    }
    const address = udpServer.address();
    log('Cámara', `Receptor UDP listo en ${address.address}:${address.port}`);
});

udpServer.on('error', (err) => {
    log('Cámara', `Error en receptor UDP: ${err.message}`, 'error');
    udpServer.close();
});

// true si a es más viejo o igual que b (soporta vuelta del contador uint32)
function isOlderOrEqual(a, b) {
    return ((a - b) | 0) <= 0;
}

function crc32(buffer) {
    let crc = 0xFFFFFFFF;
    for (const byte of buffer) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) {
            crc = (crc >>> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return (crc ^ 0xFFFFFFFF) >>> 0;
}

function finalizeFrame(frameKey, frameId) {
    const frame = framesInProgress.get(frameKey);
    if (!frame) return;
    framesInProgress.delete(frameKey);

    const { kTotal, mTotal, chunkSize, totalSize, shards, received } = frame;

    let dataShards;
    const dataComplete = shards.slice(0, kTotal).every((s) => s !== null);

    if (dataComplete) {
        // ya llegaron los kTotal paquetes de datos: no hace falta decodificar
        dataShards = shards.slice(0, kTotal);
    } else {
        dataShards = reedSolomonDecode(shards, kTotal, mTotal, chunkSize);
        if (!dataShards) {
            stats.decodeErrors++;
            return;
        }
    }

    const fullFrame = Buffer.concat(dataShards, kTotal * chunkSize).subarray(0, totalSize);

    // un JPEG válido empieza con FF D8 y termina con FF D9
    const valid =
        fullFrame.length > 4 &&
        fullFrame[0] === 0xFF && fullFrame[1] === 0xD8 &&
        fullFrame[fullFrame.length - 2] === 0xFF &&
        fullFrame[fullFrame.length - 1] === 0xD9;

    if (!valid) {
        stats.invalidJpegs++;
        return;
    }

    if (lastShownFrameId !== null && isOlderOrEqual(frameId, lastShownFrameId)) {
        // llegó tarde y ya mostramos un frame más nuevo: no vale la pena mostrarlo
        return;
    }

    if (lastShownFrameId !== null) {
        const skippedFrames = (frameId - lastShownFrameId) >>> 0;
        if (skippedFrames > 1 && skippedFrames < 0x80000000) stats.framesLost += skippedFrames - 1;
    }
    lastShownFrameId = frameId;
    stats.framesOk++;
    const reassemblyMs = Date.now() - frame.startedAt;
    stats.reassemblyMsTotal += reassemblyMs;
    stats.reassemblyMsMax = Math.max(stats.reassemblyMsMax, reassemblyMs);
    if (!dataComplete) {
        stats.framesRecovered++;
        const receivedDataShards = shards.slice(0, kTotal).filter((shard) => shard !== null).length;
        stats.dataShardsRecovered += kTotal - receivedDataShards;
    }

    // descartar frames en progreso más viejos que el que acaba de completarse
    for (const [key, pending] of framesInProgress) {
        if (pending.source === frame.source && isOlderOrEqual(pending.frameId, frameId)) {
            framesInProgress.delete(key);
        }
    }

    broadcastFrame(fullFrame);
}

udpServer.on('message', (msg, remote) => {
    stats.packetsReceived++;

    if (msg.length < UDP_HEADER_SIZE) {
        stats.invalidPackets++;
        return;
    }

    const frameId = msg.readUInt32LE(0);
    const chunkIndex = msg.readUInt16LE(4);
    const kTotal = msg.readUInt16LE(6);
    const mTotal = msg.readUInt16LE(8);
    const chunkSize = msg.readUInt16LE(10);
    const payloadSize = msg.readUInt16LE(12);
    const totalSize = msg.readUInt32LE(14);
    const checksum = msg.readUInt32LE(18);
    const total = kTotal + mTotal;

    // validaciones para descartar paquetes corruptos o ajenos
    if (kTotal === 0 || total === 0 || total > MAX_SHARDS) { stats.invalidPackets++; return; }
    if (chunkIndex >= total) { stats.invalidPackets++; return; }
    if (totalSize === 0 || totalSize > MAX_FRAME_SIZE) { stats.invalidPackets++; return; }
    // La cabecera forma parte del protocolo compartido con el firmware.
    // Rechazar tamaños incoherentes evita asignaciones grandes y frames truncados.
    if (chunkSize !== UDP_CHUNK_SIZE || mTotal !== UDP_PARITY_SHARDS) { stats.invalidPackets++; return; }
    if (kTotal > UDP_MAX_DATA_SHARDS) { stats.invalidPackets++; return; }
    if (kTotal !== Math.ceil(totalSize / UDP_CHUNK_SIZE)) { stats.invalidPackets++; return; }
    const expectedPayloadSize = chunkIndex === kTotal - 1
        ? totalSize - ((kTotal - 1) * UDP_CHUNK_SIZE)
        : UDP_CHUNK_SIZE;
    if (payloadSize !== expectedPayloadSize || payloadSize < 1 || payloadSize > chunkSize) { stats.invalidPackets++; return; }
    if (msg.length !== UDP_HEADER_SIZE + payloadSize) { stats.invalidPackets++; return; }

    const payload = msg.subarray(UDP_HEADER_SIZE);
    if (crc32(payload) !== checksum) {
        stats.crcErrors++;
        return;
    }

    const now = Date.now();
    stats.packetsAccepted++;

    // si el ESP32 se reinició (pasó un rato sin paquetes), su frameId vuelve a 0:
    // resetear la referencia
    if (now - lastPacketTime > ESP_RESET_TIMEOUT_MS) {
        lastShownFrameId = null;
        framesInProgress.clear();
    }
    lastPacketTime = now;

    // ignorar chunks de frames más viejos que el último mostrado
    if (lastShownFrameId !== null && isOlderOrEqual(frameId, lastShownFrameId)) {
        return;
    }

    const source = `${remote.address}:${remote.port}`;
    const frameKey = `${source}:${frameId}`;
    let frame = framesInProgress.get(frameKey);
    if (!frame) {
        if (framesInProgress.size >= MAX_FRAMES_IN_PROGRESS) {
            const oldestFrameKey = framesInProgress.keys().next().value;
            framesInProgress.delete(oldestFrameKey);
            stats.framesEvicted++;
        }
        frame = {
            frameId,
            source,
            kTotal,
            mTotal,
            chunkSize,
            totalSize,
            shards: new Array(total).fill(null),
            received: 0,
            startedAt: now,
            lastUpdate: now,
        };
        framesInProgress.set(frameKey, frame);
    }

    // el chunk debe ser coherente con el frame que ya estamos armando
    if (frame.kTotal !== kTotal || frame.mTotal !== mTotal || frame.totalSize !== totalSize) {
        stats.invalidPackets++;
        return;
    }

    if (frame.shards[chunkIndex] === null) {
        // copia el payload: msg puede ser reutilizado internamente por dgram
        const shard = Buffer.alloc(chunkSize);
        payload.copy(shard);
        frame.shards[chunkIndex] = shard;
        frame.received++;
        frame.lastUpdate = now;
    } else {
        stats.duplicatePackets++;
    }

    // apenas hay kTotal paquetes disponibles (de datos y/o paridad) se puede
    // reconstruir: no hace falta esperar los kTotal+mTotal para mantener
    // la latencia baja.
    if (frame.received >= frame.kTotal) {
        finalizeFrame(frameKey, frameId);
    }
});

// limpieza de frames que nunca llegaron a kTotal paquetes (paquetes UDP perdidos)
const frameCleanupInterval = setInterval(() => {
    const now = Date.now();
    for (const [frameId, frame] of framesInProgress) {
        if (now - frame.lastUpdate > UDP_FRAME_TIMEOUT_MS) {
            framesInProgress.delete(frameId);
            stats.framesTimedOut++;
        }
    }
}, 100);

udpServer.bind(UDP_PORT);
// ---------- FIN UDP ----------

// Stream MJPEG: se puede abrir directo en el navegador o usar en <img src="...">
app.get('/camera/stream', (req, res) => {

    res.writeHead(200, {
        'Content-Type': 'multipart/x-mixed-replace; boundary=frame',
        'Access-Control-Allow-Origin': '*',
        'Cache-Control': 'no-cache, no-store, must-revalidate',
        'Pragma': 'no-cache',
        'Connection': 'keep-alive'
    });

    // enviar cada frame apenas se escribe, sin agruparlos
    if (res.socket) res.socket.setNoDelay(true);

    cameraClients.add(res);

    if (latestFrame) {
        res.write(
            `--frame\r\n` +
            `Content-Type: image/jpeg\r\n` +
            `Content-Length: ${latestFrame.length}\r\n\r\n`
        );
        res.write(latestFrame);
        res.write('\r\n');
    }

    req.on('close', () => {
        cameraClients.delete(res);
    });
});

// Última imagen como JPEG suelto (útil para probar o hacer capturas)
app.get('/camera/snapshot', (req, res) => {
    if (!latestFrame) {
        return res.status(503).send('Aún no se recibieron frames del ESP32-CAM');
    }
    res.set({
        'Content-Type': 'image/jpeg',
        'Content-Length': latestFrame.length,
        'Access-Control-Allow-Origin': '*',
        'Cache-Control': 'no-cache, no-store, must-revalidate'
    });
    res.send(latestFrame);
});
// ---------- FIN CÁMARA ----------

// iniciando mqtt y websocket
clientmqtt.on("connect", () => {
    log('MQTT', 'Conexión establecida con el broker mqtt://localhost:1883.')
    clientmqtt.subscribe([
        'sensor/movimiento',
        'actuador/cerrojo',
        'sensor/cerrojo'
    ], (error) => {
        if (error) log('MQTT', `No se pudieron suscribir los tópicos: ${error.message}`, 'error')
        else log('MQTT', 'Suscrito a sensor/movimiento, actuador/cerrojo y sensor/cerrojo.')
    })
});
clientmqtt.on('reconnect', () => log('MQTT', 'Se perdió la conexión; intentando reconectar con el broker.', 'warn'))
clientmqtt.on('offline', () => log('MQTT', 'Broker desconectado; los comandos no llegarán al ESP32 hasta reconectar.', 'warn'))
clientmqtt.on('error', (error) => log('MQTT', `Error de conexión: ${error.message}`, 'error'))

// recibiendo mensajes en MQTT
clientmqtt.on("message", async (topic, msg) => {
    if (topic == "sensor/movimiento") {
        let estado = msg.toString();
        log('Sensor', `Movimiento: ${estado === 'ON' ? 'detectado' : 'sin movimiento'}.`)
        socketServers.forEach((socketServer) => socketServer.emit('movimiento', estado))
        if (estado == "ON") {
            updateSensorState.run(1, 'movimiento')
        } else{
            updateSensorState.run(0, 'movimiento')
        }
    } else if (topic == "sensor/cerrojo") {
        let estado = msg.toString()
        log('Cerrojo', `Estado informado por el ESP32: ${estado === 'ON' ? 'cerrado' : 'abierto'}.`)
        socketServers.forEach((socketServer) => socketServer.emit('cerrojo', estado))
        if (estado == "ON") {
            updateSensorState.run(1, 'cerrojo')
        } else {
            updateSensorState.run(0, 'cerrojo')
        }
    }
});

function handleSocketConnection(socket) {
    const sensors = db.prepare('SELECT nombre, estado FROM sensores').all()
    sensors.forEach((sensor)=>{
        if(sensor.nombre=="cerrojo"){
            if(sensor.estado){ socket.emit('cerrojo', 'ON') }
            else{ socket.emit('cerrojo', 'OFF') }
        }
        if(sensor.nombre=="movimiento"){
            if(sensor.estado){ socket.emit('movimiento', 'ON') }
            else{ socket.emit('movimiento', 'OFF') }
        }
    })
    socket.on('cerrar', () => {
        clientmqtt.publish('actuador/cerrojo', 'CERRAR', (error) => {
            if (error) log('Cerrojo', `No se pudo enviar el comando CERRAR: ${error.message}`, 'error')
            else log('Cerrojo', 'Comando CERRAR enviado al ESP32 por MQTT.')
        })
    })
    socket.on('abrir', () => {
        clientmqtt.publish('actuador/cerrojo', 'ABRIR', (error) => {
            if (error) log('Cerrojo', `No se pudo enviar el comando ABRIR: ${error.message}`, 'error')
            else log('Cerrojo', 'Comando ABRIR enviado al ESP32 por MQTT.')
        })
    })
}

io.on('connection', handleSocketConnection)

// Publica la interfaz web por HTTP.
const httpPort = Number(process.env.HTTP_PORT) || 80
const httpWebServer = createServer(app)
const httpIo = new Server(httpWebServer, { cors: { origin: '*' } })
socketServers.push(httpIo)
httpIo.on('connection', handleSocketConnection)
let cloudflaredProcess = null

function startCloudflareTunnel() {
    const cloudflaredBin = 'cloudflared' // Se resuelve desde PATH para que el proyecto sea portable.
    const localUrl = `http://127.0.0.1:${httpPort}`
    const args = ['tunnel', '--no-autoupdate', '--url', localUrl]
    log('Cloudflare', `Iniciando Quick Tunnel hacia ${localUrl}...`)
    cloudflaredProcess = spawn(cloudflaredBin, args, { stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true })

    let output = ''
    const reportOutput = (chunk) => {
        const line = chunk.toString()
        output = (output + line).slice(-12000)
        const url = output.match(/https:\/\/[-a-z0-9]+\.trycloudflare\.com/i)
        if (url) {
            log('Cloudflare', `Túnel Quick listo: ${url[0]}`)
            output = ''
        }
    }
    cloudflaredProcess.stdout.on('data', reportOutput)
    cloudflaredProcess.stderr.on('data', reportOutput)
    cloudflaredProcess.on('error', (error) => {
        log('Cloudflare', `No se pudo iniciar cloudflared (${cloudflaredBin}): ${error.message}`, 'error')
    })
    cloudflaredProcess.on('exit', (code, signal) => {
        cloudflaredProcess = null
        if (code !== 0 && code !== null) log('Cloudflare', `El túnel terminó con código ${code}.`, 'error')
        else if (signal) log('Cloudflare', `El túnel se detuvo (señal ${signal}).`, 'warn')
    })
}

httpWebServer.listen(httpPort, () => {
    log('Web', `Panel disponible en http://localhost:${httpPort}.`)
    const localAddresses = Object.values(networkInterfaces()).flat()
        .filter((address) => address && address.family === 'IPv4' && !address.internal)
        .map((address) => address.address)
    for (const address of localAddresses) {
        log('Web', `IP local: http://${address}:${httpPort}`)
    }
    startCloudflareTunnel()
})

let isShuttingDown = false
async function shutdown(signal) {
    if (isShuttingDown) return
    isShuttingDown = true
    log('Web', `Recibida señal ${signal}; cerrando servicios...`, 'warn')
    clearInterval(cameraStatsInterval)
    clearInterval(frameCleanupInterval)
    clearTimeout(frameTimer)

    const closeServer = (ioServer) => new Promise((resolveClose) => {
        try { ioServer.close(() => resolveClose()) } catch { resolveClose() }
    })
    const closeUdp = new Promise((resolveClose) => {
        try {
            if (!udpServer.address()) return resolveClose()
            udpServer.close(() => resolveClose())
        } catch { resolveClose() }
    })
    const closeMqtt = new Promise((resolveClose) => {
        try { clientmqtt.end(true, () => resolveClose()) } catch { resolveClose() }
    })
    const closeTunnel = new Promise((resolveClose) => {
        const child = cloudflaredProcess
        if (!child || child.exitCode !== null) return resolveClose()
        child.once('exit', resolveClose)
        child.kill()
    })
    const forceExitTimer = setTimeout(() => {
        log('Web', 'Se agotó el tiempo de cierre; terminando el proceso.', 'error')
        process.exit(1)
    }, 5000)

    await Promise.allSettled([
        closeServer(io),
        closeServer(httpIo),
        closeUdp,
        closeMqtt,
        closeTunnel
    ])
    clearTimeout(forceExitTimer)
    log('Web', 'Servidor, MQTT, UDP y túnel detenidos.')
    process.exit(0)
}
process.on('SIGINT', () => shutdown('Ctrl+C'))
process.on('SIGTERM', () => shutdown('SIGTERM'))

server.listen(port, () => {
    log('WebSocket', `Servicio local listo en http://localhost:${port}.`)
})
