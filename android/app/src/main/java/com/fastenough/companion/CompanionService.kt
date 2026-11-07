package com.fastenough.companion

import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.ConnectivityManager
import android.net.Network
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import org.json.JSONObject

/** Foreground service that hosts the file server while the phone waits for a computer. */
class CompanionService : Service(), NativeBridge.Host {
    companion object {
        private const val TAG = "FastEnoughService"
        const val ACTION_START = "com.fastenough.companion.START"
        const val ACTION_STOP = "com.fastenough.companion.STOP"
        // Port 5740 belongs to the helper that the PC starts over ADB, so the app uses its own range.
        private val PORTS = (5750..5759).toList()

        fun start(context: Context) {
            context.startForegroundService(Intent(context, CompanionService::class.java).setAction(ACTION_START))
        }

        fun stop(context: Context) {
            context.startService(Intent(context, CompanionService::class.java).setAction(ACTION_STOP))
        }
    }

    private var serverPort = 0
    private var discovery: Discovery? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var networkCallback: ConnectivityManager.NetworkCallback? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        CompanionSettings.init(this)
        Notifications.createChannels(this)
        startInForeground()
        NativeBridge.host = this
        NativeBridge.ensureInitialized()
        startServer()
        discovery = Discovery(this, serverPort).also { it.start() }
        multicastLock = getSystemService(WifiManager::class.java)?.createMulticastLock("fastenough-discovery")?.apply {
            setReferenceCounted(false)
            acquire()
        }
        watchNetworks()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                CompanionSettings.serverEnabled = false
                stopSelf()
                return START_NOT_STICKY
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        NativeBridge.nativeStopServer()
        RootHelper.stop()
        NativeBridge.host = null
        discovery?.stop()
        multicastLock?.release()
        releaseActivityLocks()
        networkCallback?.let { runCatching { getSystemService(ConnectivityManager::class.java).unregisterNetworkCallback(it) } }
        CompanionState.update { it.copy(serverRunning = false, clients = 0) }
        super.onDestroy()
    }

    private fun startInForeground() {
        val notification = buildNotification()
        if (Build.VERSION.SDK_INT >= 29)
            startForeground(Notifications.SERVICE_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        else startForeground(Notifications.SERVICE_ID, notification)
    }

    private fun startServer() {
        var lastError = 0
        for (port in PORTS) {
            val error = NativeBridge.nativeStartServer(port)
            if (error == 0) {
                serverPort = port
                CompanionState.update { it.copy(serverRunning = true, serverError = "", addresses = Discovery.addresses()) }
                Log.i(TAG, "File server listening on port $port")
                updateNotification()
                return
            }
            lastError = error
        }
        CompanionState.update { it.copy(serverRunning = false, serverError = "Could not open a network port (error $lastError)") }
        updateNotification()
    }

    private fun watchNetworks() {
        val connectivity = getSystemService(ConnectivityManager::class.java) ?: return
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) = refreshAddresses()
            override fun onLost(network: Network) = refreshAddresses()
            override fun onLinkPropertiesChanged(network: Network, linkProperties: android.net.LinkProperties) = refreshAddresses()
        }
        runCatching { connectivity.registerDefaultNetworkCallback(callback) }
        runCatching {
            connectivity.registerNetworkCallback(android.net.NetworkRequest.Builder().build(), callback)
        }
        networkCallback = callback
    }

    fun refreshAddresses() {
        CompanionState.update { it.copy(addresses = Discovery.addresses()) }
        updateNotification()
    }

    // --- Native callbacks ---

    override fun authorize(request: String): String {
        val json = runCatching { JSONObject(request) }.getOrNull() ?: return "NInvalid request"
        val clientId = json.optString("client")
        val name = json.optString("name").ifBlank { "Computer" }.take(64)
        val provided = json.optString("token")
        if (clientId.isBlank()) return "NMissing computer identity"

        val trusted = CompanionSettings.findTrusted(clientId)
        val token = if (trusted != null && CompanionSettings.tokenMatches(trusted.token, provided)) {
            CompanionSettings.touch(clientId, name)
            trusted.token
        } else {
            if (json.optBoolean("noPrompt")) return "NThis computer is not paired with the phone"
            Approvals.request(this, clientId, name) ?: return "NThe phone did not allow this computer"
        }

        val staging = getExternalFilesDir("staging")?.also { it.mkdirs() }?.absolutePath ?: ""
        val reply = JSONObject()
            .put("device", CompanionSettings.deviceId)
            .put("name", CompanionSettings.deviceName(this))
            .put("model", Build.MODEL)
            .put("manufacturer", Build.MANUFACTURER)
            .put("sdk", Build.VERSION.SDK_INT)
            .put("token", token)
            .put("storage", Permissions.hasStorageAccess(this))
            .put("staging", staging)
            .put("companion", 1)
        val wantsRoot = json.optBoolean("root") && CompanionSettings.allowRoot
        return (if (wantsRoot) "R" else "Y") + reply.toString()
    }

    override fun appCommand(command: Int, payload: String): String = try {
        when (command) {
            28 -> "Y" + AppCommands.listApps(this)
            29 -> AppCommands.install(this, payload).let { (ok, text) -> (if (ok) "Y" else "N") + text }
            30 -> AppCommands.uninstall(this, payload).let { (ok, text) -> (if (ok) "Y" else "N") + text }
            else -> "NUnknown request"
        }
    } catch (error: Exception) {
        "N" + (error.message ?: "Request failed")
    }

    override fun launchRootHelper(name: String, secret: String): Boolean =
        CompanionSettings.allowRoot && RootHelper.launch(this, name, secret)

    override fun clientsChanged(count: Int) {
        CompanionState.update { it.copy(clients = count) }
        if (count > 0) acquireActivityLocks() else releaseActivityLocks()
        updateNotification()
    }

    // --- Locks and notification ---

    @Synchronized
    private fun acquireActivityLocks() {
        if (wakeLock == null) {
            wakeLock = getSystemService(PowerManager::class.java)
                ?.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "FastEnough:transfer")?.apply {
                    setReferenceCounted(false)
                    acquire(6 * 60 * 60 * 1000L)
                }
        }
        if (wifiLock == null) {
            @Suppress("DEPRECATION")
            val mode = if (Build.VERSION.SDK_INT >= 29) WifiManager.WIFI_MODE_FULL_LOW_LATENCY else WifiManager.WIFI_MODE_FULL_HIGH_PERF
            wifiLock = getSystemService(WifiManager::class.java)?.createWifiLock(mode, "FastEnough:transfer")?.apply {
                setReferenceCounted(false)
                acquire()
            }
        }
    }

    @Synchronized
    private fun releaseActivityLocks() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
    }

    private fun buildNotification(): Notification {
        val status = CompanionState.status.value
        val title = when {
            !status.serverRunning && status.serverError.isNotEmpty() -> "File access unavailable"
            status.clients > 0 -> "Computer connected"
            else -> "Ready for your computer"
        }
        val links = mutableListOf<String>()
        status.addresses.firstOrNull { it.kind == "wifi" }?.let { links += "WiFi ${it.ip}" }
        status.addresses.firstOrNull { it.kind == "usb" }?.let { links += "USB ${it.ip}" }
        val text = if (status.serverError.isNotEmpty()) status.serverError
            else links.joinToString(" · ").ifEmpty { "Connect to WiFi or plug in a USB cable" }
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val stop = PendingIntent.getService(this, 1, Intent(this, CompanionService::class.java).setAction(ACTION_STOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        return Notifications.builder(this, Notifications.CHANNEL_SERVICE)
            .setContentTitle(title)
            .setContentText(text)
            .setOngoing(true)
            .setContentIntent(open)
            .addAction(Notification.Action.Builder(null, "Stop", stop).build())
            .build()
    }

    private fun updateNotification() {
        runCatching {
            getSystemService(android.app.NotificationManager::class.java).notify(Notifications.SERVICE_ID, buildNotification())
        }
    }
}
