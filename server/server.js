import express from 'express'
import { Server } from 'socket.io'
import { createServer } from 'http'
import cors from "cors";
import mqtt from 'mqtt'
import mysql from 'mysql2/promise'

// variables de servidor express
const port = 3000
const app = express()
const server = createServer(app)
// base de datos
const db = mysql.createPool({
    host: process.env.DB_HOST || 'localhost',
    user: process.env.DB_USER || 'root',
    password: process.env.DB_PASS || '',
    database: process.env.DB_NAME || 'bridgearcson',
    connectionLimit: 10
})
// mqtt
const clientmqtt = mqtt.connect("mqtt://localhost:1883");
// websocket
const io = new Server(server, {
    cors: {
        origin: "*"
    }
})
// iniciando mqtt y websocket
clientmqtt.on("connect", () => {
    clientmqtt.subscribe([
        'sensor/movimiento',
        'actuador/cerrojo',
        'sensor/cerrojo'
    ])
    console.log("Conectado a MQTT");
    console.log("subcriptos los topicos correctamente");
});

// recibiendo mensajes en MQTT
clientmqtt.on("message", async (topic, msg) => {
    if (topic == "sensor/movimiento") {
        let estado = msg.toString();
        io.emit('movimiento', estado)
        if (estado == "ON") {
            await db.query("UPDATE sensores SET estado = TRUE WHERE nombre = 'movimiento';")
        } else{
            await db.query("UPDATE sensores SET estado = FALSE WHERE nombre = 'movimiento';")
        }
    } else if (topic == "sensor/cerrojo") {
        let estado = msg.toString()
        io.emit('cerrojo', estado)
        if (estado == "ON") {
            await db.query("UPDATE sensores SET estado = TRUE WHERE nombre = 'cerrojo';")
        } else {
            await db.query("UPDATE sensores SET estado = FALSE WHERE nombre = 'cerrojo';")
        }
    }
});

io.on('connection', async (socket) => {
    const rows = await db.query(
        'SELECT nombre, estado FROM sensores'
    )
    rows[0].forEach((sensor)=>{
        console.log(sensor)
        if(sensor.nombre=="cerrojo"){
            if(sensor.estado){ io.emit('cerrojo', 'ON') }
            else{ io.emit('cerrojo', 'OFF') }
        }
        if(sensor.nombre=="movimiento"){
            if(sensor.estado){ io.emit('movimiento', 'ON') }
            else{ io.emit('movimiento', 'OFF') }
        }
    })
    socket.on('cerrar', () => {
        clientmqtt.publish('sensor/cerrojo', "ON")
    })
    socket.on('abrir', () => {
        clientmqtt.publish('sensor/cerrojo', "OFF")
    })
})
server.listen(3000, (req, res) => {
    console.log(`server on port ${port}`)
})

