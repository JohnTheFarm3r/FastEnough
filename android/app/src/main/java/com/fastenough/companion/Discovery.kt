package com.fastenough.companion

import android.content.Context
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.NetworkInterface
import kotlin.concurrent.thread

/** Answers discovery broadcasts from computers on the same network or USB link. */
class Discovery(private val context: Context, private val serverPort: Int) {
    companion object {
        const val PORT = 5745
        private const val TAG = "FastEnoughDiscovery"

        fun interfaceKind(name: String): String = when {
            name.startsWith("wlan") || name.startsWith("swlan") || name.startsWith("ap") -> "wifi"
            name.startsWith("rndis") || name.startsWith("usb") || name.startsWith("ncm") -> "usb"
            name.startsWith("eth") -> "ethernet"
            else -> "other"
        }

        /** Lists the IPv4 addresses a computer can use to reach this phone. */
        fun addresses(): List<CompanionState.Address> {
            val result = mutableListOf<CompanionState.Address>()
            val interfaces = runCatching { NetworkInterface.getNetworkInterfaces()?.toList() }.getOrNull() ?: return result
            for (nic in interfaces) {
                if (runCatching { !nic.isUp || nic.isLoopback }.getOrDefault(true)) continue
                val kind = interfaceKind(nic.name)
                // Mobile data and VPN links are not reachable from the computer.
                if (kind == "other") continue
                for (address in nic.inetAddresses) {
                    if (address is Inet4Address && !address.isLoopbackAddress)
                        result += CompanionState.Address(address.hostAddress ?: continue, kind, nic.name)
                }
            }
            return result
        }
    }

    @Volatile private var socket: DatagramSocket? = null

    fun start() {
        if (socket != null) return
        val bound = try {
            DatagramSocket(null).apply {
                reuseAddress = true
                broadcast = true
                bind(InetSocketAddress(PORT))
            }
        } catch (error: Exception) {
            Log.w(TAG, "Discovery port unavailable: ${error.message}")
            return
        }
        socket = bound
        thread(name = "discovery", isDaemon = true) { serve(bound) }
    }

    fun stop() {
        socket?.close()
        socket = null
    }

    private fun serve(socket: DatagramSocket) {
        val buffer = ByteArray(2048)
        while (!socket.isClosed) {
            val packet = DatagramPacket(buffer, buffer.size)
            try {
                socket.receive(packet)
            } catch (_: Exception) {
                break
            }
            val text = String(packet.data, packet.offset, packet.length, Charsets.UTF_8)
            val request = runCatching { JSONObject(text) }.getOrNull() ?: continue
            if (request.optString("type") != "afm-discover") continue
            val reply = describe().toString().toByteArray(Charsets.UTF_8)
            runCatching { socket.send(DatagramPacket(reply, reply.size, packet.socketAddress)) }
        }
    }

    private fun describe(): JSONObject {
        val list = JSONArray()
        addresses().forEach { list.put(JSONObject().put("ip", it.ip).put("kind", it.kind).put("iface", it.iface)) }
        return JSONObject()
            .put("type", "afm-device")
            .put("version", 1)
            .put("id", CompanionSettings.deviceId)
            .put("name", CompanionSettings.deviceName(context))
            .put("model", android.os.Build.MODEL)
            .put("manufacturer", android.os.Build.MANUFACTURER)
            .put("port", serverPort)
            .put("addresses", list)
            .put("storage", Permissions.hasStorageAccess(context))
    }
}
