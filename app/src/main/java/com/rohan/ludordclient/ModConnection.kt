package com.rohan.ludordclient

import android.net.LocalSocket
import android.net.LocalSocketAddress
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.IOException

class ModConnection {
    private var socket: LocalSocket? = null
    private var writer: BufferedWriter? = null
    private var reader: BufferedReader? = null

    var isConnected = false
        private set

    fun connect(socketName: String = "ludord.mod.socket"): Boolean {
        return try {
            socket = LocalSocket()
            socket!!.connect(LocalSocketAddress(socketName))
            writer = socket!!.outputStream.bufferedWriter()
            reader = socket!!.inputStream.bufferedReader()
            isConnected = true
            true
        } catch (e: IOException) {
            isConnected = false
            false
        }
    }

    fun sendCommand(cmd: String): String {
        return try {
            writer?.write("$cmd\n")
            writer?.flush()
            reader?.readLine() ?: "NO_RESPONSE"
        } catch (e: IOException) {
            isConnected = false
            "ERROR"
        }
    }

    fun ping(): Boolean = sendCommand("PING") == "PONG"
    fun setRedWin(v: Boolean) = sendCommand("RED_WIN:$v")
    fun setDice(v: Int) = sendCommand("SET_DICE:$v")
    fun setAiDumb(v: Boolean) = sendCommand("AI_DUMB:$v")
    fun getState() = sendCommand("GET_STATE")

    fun disconnect() {
        try { socket?.close() } catch (_: Exception) {}
        isConnected = false
    }
}
