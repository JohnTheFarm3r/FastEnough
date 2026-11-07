package com.fastenough.companion

import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInstaller
import android.os.Build
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

/** Lists, installs and removes apps for the connected computer. */
object AppCommands {
    const val ACTION_RESULT = "com.fastenough.companion.PACKAGE_RESULT"
    const val EXTRA_REQUEST = "request"
    private const val TIMEOUT_MINUTES = 10L

    private class Request {
        val latch = CountDownLatch(1)
        @Volatile var status = PackageInstaller.STATUS_FAILURE
        @Volatile var message = ""
    }

    private val requests = ConcurrentHashMap<Int, Request>()
    private val nextRequest = AtomicInteger(1)

    fun listApps(context: Context): String {
        val pm = context.packageManager
        val apps = JSONArray()
        @Suppress("DEPRECATION")
        val packages = pm.getInstalledPackages(0)
        for (info in packages) {
            val app = info.applicationInfo ?: continue
            val system = app.flags and (ApplicationInfo.FLAG_SYSTEM or ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) != 0
            val label = runCatching { app.loadLabel(pm).toString() }.getOrDefault(info.packageName)
            val versionCode = if (Build.VERSION.SDK_INT >= 28) info.longVersionCode else {
                @Suppress("DEPRECATION") info.versionCode.toLong()
            }
            val splits = JSONArray()
            app.splitSourceDirs?.forEach { splits.put(it) }
            apps.put(JSONObject()
                .put("package", info.packageName)
                .put("label", label)
                .put("path", app.sourceDir ?: "")
                .put("splits", splits)
                .put("system", system)
                .put("versionName", info.versionName ?: "")
                .put("versionCode", versionCode))
        }
        return apps.toString()
    }

    fun install(context: Context, payload: String): Pair<Boolean, String> {
        val paths = runCatching {
            val array = JSONObject(payload).getJSONArray("paths")
            (0 until array.length()).map { array.getString(it) }
        }.getOrElse { return false to "Invalid install request" }
        if (paths.isEmpty()) return false to "No APK files were provided"
        val installer = context.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        if (Build.VERSION.SDK_INT >= 31)
            params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        val sessionId = try {
            installer.createSession(params)
        } catch (error: Exception) {
            return false to (error.message ?: "Could not start the installation")
        }
        try {
            installer.openSession(sessionId).use { session ->
                paths.forEachIndexed { index, path ->
                    val file = File(path)
                    if (!file.isFile) throw IllegalArgumentException("Missing APK: ${file.name}")
                    session.openWrite("part$index.apk", 0, file.length()).use { output ->
                        file.inputStream().use { it.copyTo(output, 1 shl 20) }
                        session.fsync(output)
                    }
                }
                val requestId = nextRequest.getAndIncrement()
                val request = Request().also { requests[requestId] = it }
                session.commit(resultSender(context, requestId))
                return await(requestId, request)
            }
        } catch (error: Exception) {
            runCatching { installer.abandonSession(sessionId) }
            return false to (error.message ?: "Installation failed")
        }
    }

    fun uninstall(context: Context, packageName: String): Pair<Boolean, String> {
        if (packageName.isBlank()) return false to "No package was provided"
        val requestId = nextRequest.getAndIncrement()
        val request = Request().also { requests[requestId] = it }
        return try {
            context.packageManager.packageInstaller.uninstall(packageName, resultSender(context, requestId))
            await(requestId, request)
        } catch (error: Exception) {
            requests.remove(requestId)
            false to (error.message ?: "Uninstall failed")
        }
    }

    private fun resultSender(context: Context, requestId: Int) = PendingIntent.getBroadcast(
        context, requestId,
        Intent(context, PackageResultReceiver::class.java).setAction(ACTION_RESULT).putExtra(EXTRA_REQUEST, requestId),
        PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT).intentSender

    private fun await(requestId: Int, request: Request): Pair<Boolean, String> {
        val done = request.latch.await(TIMEOUT_MINUTES, TimeUnit.MINUTES)
        requests.remove(requestId)
        if (!done) return false to "No answer on the phone"
        return when (request.status) {
            PackageInstaller.STATUS_SUCCESS -> true to "{}"
            PackageInstaller.STATUS_FAILURE_ABORTED -> false to "Cancelled on the phone"
            else -> false to request.message.ifBlank { "The phone reported an error" }
        }
    }

    fun onResult(context: Context, intent: Intent) {
        val requestId = intent.getIntExtra(EXTRA_REQUEST, -1)
        val request = requests[requestId] ?: return
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            @Suppress("DEPRECATION")
            val confirm = intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT) ?: return
            askUser(context, requestId, confirm)
            return
        }
        context.getSystemService(NotificationManager::class.java).cancel(1000 + requestId)
        request.status = status
        request.message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: ""
        request.latch.countDown()
    }

    private fun askUser(context: Context, requestId: Int, confirm: Intent) {
        confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (MainActivity.visible) {
            runCatching { context.startActivity(confirm); return }
        }
        val open = PendingIntent.getActivity(context, 1000 + requestId, confirm,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val notification = Notifications.builder(context, Notifications.CHANNEL_ACTIONS)
            .setContentTitle("Confirm on this phone")
            .setContentText("Your computer asked to install or remove an app. Tap to review.")
            .setContentIntent(open)
            .setAutoCancel(true)
            .setCategory(android.app.Notification.CATEGORY_RECOMMENDATION)
            .build()
        runCatching { context.getSystemService(NotificationManager::class.java).notify(1000 + requestId, notification) }
        // Opening the prompt directly works when the app is allowed to start activities.
        runCatching { context.startActivity(confirm) }
    }

    fun hasUnknownSourcesAccess(context: Context): Boolean =
        context.packageManager.canRequestPackageInstalls()
}

class PackageResultReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == AppCommands.ACTION_RESULT) AppCommands.onResult(context, intent)
    }
}
