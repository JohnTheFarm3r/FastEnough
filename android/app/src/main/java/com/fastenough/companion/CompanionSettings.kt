package com.fastenough.companion

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import android.provider.Settings
import org.json.JSONObject
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.UUID

/** Persistent app settings and the list of computers allowed to connect. */
object CompanionSettings {
    private lateinit var prefs: SharedPreferences
    private val random = SecureRandom()

    fun init(context: Context) {
        if (::prefs.isInitialized) return
        prefs = context.getSharedPreferences("companion", Context.MODE_PRIVATE)
        if (!prefs.contains("deviceId")) prefs.edit().putString("deviceId", UUID.randomUUID().toString()).apply()
    }

    val deviceId: String get() = prefs.getString("deviceId", "") ?: ""

    var serverEnabled: Boolean
        get() = prefs.getBoolean("serverEnabled", true)
        set(value) = prefs.edit().putBoolean("serverEnabled", value).apply()

    var startOnBoot: Boolean
        get() = prefs.getBoolean("startOnBoot", true)
        set(value) = prefs.edit().putBoolean("startOnBoot", value).apply()

    var allowRoot: Boolean
        get() = prefs.getBoolean("allowRoot", true)
        set(value) = prefs.edit().putBoolean("allowRoot", value).apply()

    fun deviceName(context: Context): String {
        val name = runCatching { Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME) }.getOrNull()
        if (!name.isNullOrBlank()) return name
        val model = Build.MODEL ?: "Android"
        val maker = Build.MANUFACTURER ?: ""
        return if (model.startsWith(maker, ignoreCase = true)) model else "$maker $model".trim()
    }

    data class TrustedComputer(val id: String, val name: String, val token: String, val added: Long, val lastSeen: Long)

    @Synchronized
    fun trustedComputers(): List<TrustedComputer> {
        val root = JSONObject(prefs.getString("trusted", "{}") ?: "{}")
        return root.keys().asSequence().map { id ->
            val entry = root.getJSONObject(id)
            TrustedComputer(id, entry.optString("name"), entry.optString("token"),
                entry.optLong("added"), entry.optLong("lastSeen"))
        }.sortedByDescending { it.lastSeen }.toList()
    }

    @Synchronized
    fun findTrusted(clientId: String): TrustedComputer? = trustedComputers().firstOrNull { it.id == clientId }

    /** Adds or replaces a trusted computer and returns its new access token. */
    @Synchronized
    fun trust(clientId: String, name: String): String {
        val bytes = ByteArray(32).also { random.nextBytes(it) }
        val token = bytes.joinToString("") { "%02x".format(it) }
        val root = JSONObject(prefs.getString("trusted", "{}") ?: "{}")
        val now = System.currentTimeMillis()
        root.put(clientId, JSONObject().put("name", name).put("token", token).put("added", now).put("lastSeen", now))
        prefs.edit().putString("trusted", root.toString()).apply()
        CompanionState.refreshTrusted()
        return token
    }

    @Synchronized
    fun touch(clientId: String, name: String) {
        val root = JSONObject(prefs.getString("trusted", "{}") ?: "{}")
        val entry = root.optJSONObject(clientId) ?: return
        entry.put("lastSeen", System.currentTimeMillis())
        if (name.isNotBlank()) entry.put("name", name)
        prefs.edit().putString("trusted", root.toString()).apply()
    }

    @Synchronized
    fun forget(clientId: String) {
        val root = JSONObject(prefs.getString("trusted", "{}") ?: "{}")
        root.remove(clientId)
        prefs.edit().putString("trusted", root.toString()).apply()
        CompanionState.refreshTrusted()
    }

    fun tokenMatches(expected: String, provided: String): Boolean =
        expected.isNotEmpty() && MessageDigest.isEqual(expected.toByteArray(), provided.toByteArray())
}
