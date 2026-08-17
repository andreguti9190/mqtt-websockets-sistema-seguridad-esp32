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

// recibiendo mensajes
clientmqtt.on("message", (topic, msg) => {
    if (topic == "sensor/movimiento") {
        io.emit('movimiento', msg.toString())
    } else if (topic == "sensor/cerrojo") {
        let estado = msg.toString()
        if (estado == "ON") {
            io.emit('cerrojo', 'cerrado')
        } else {
            io.emit('cerrojo', 'abierto')
        }
    }
});

io.on('connection', async (socket) => {
    const rows = await db.query(
        'SELECT nombre, estado FROM sensores'
    )
    rows[0].forEach((sensor)=>{
        if(sensor.nombre=="cerrojo"){
            if(sensor.estado){ io.emit('cerrojo', 'abierto') }
            else{ io.emit('movimiento', 'cerrado') }
        }
        if(sensor.nombre=="movimiento"){
            if(sensor.estado){ io.emit('cerrojo', 'abierto') }
            else{ io.emit('movimiento', 'cerrado') }
        }
    })
    socket.on('cerrar', () => {
        clientmqtt.publish('actuador/cerrojo', "ON")
        console.log('cerrado')
    })
    socket.on('abrir', () => {
        clientmqtt.publish('actuador/cerrojo', "OFF")
        console.log('abrir')
    })
    socket.on('disconnect', () => {
        console.log('Un usuario desconectado')
    })
})
server.listen(3000, (req, res) => {
    console.log(`server on port ${port}`)
})

