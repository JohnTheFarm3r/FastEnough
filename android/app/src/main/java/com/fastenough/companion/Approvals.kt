package com.fastenough.companion

import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Asks the user whether a new computer may connect, and waits for the answer. */
object Approvals {
    private const val TIMEOUT_SECONDS = 90L
    const val ACTION_DECIDE = "com.fastenough.companion.DECIDE"
    const val EXTRA_CLIENT = "client"
    const val EXTRA_ALLOW = "allow"

    private class Pending(val clientId: String, val name: String, val notificationId: Int) {
        val latch = CountDownLatch(1)
        @Volatile var token: String? = null
    }

    private val pending = LinkedHashMap<String, Pending>()
    private var nextNotificationId = 100

    /** Blocks until the user decides. Returns the new token, or null when denied. */
    fun request(context: Context, clientId: String, name: String): String? {
        val entry: Pending
        val created: Boolean
        synchronized(this) {
            val existing = pending[clientId]
            created = existing == null
            entry = existing ?: Pending(clientId, name, nextNotificationId++).also { pending[clientId] = it }
            publish()
        }
        if (created) showNotification(context, entry)
        val decided = entry.latch.await(TIMEOUT_SECONDS, TimeUnit.SECONDS)
        if (!decided) resolve(context, clientId, false)
        return entry.token
    }

    fun resolve(context: Context, clientId: String, allow: Boolean) {
        val entry: Pending
        synchronized(this) {
            entry = pending.remove(clientId) ?: return
            publish()
        }
        if (allow) entry.token = CompanionSettings.trust(clientId, entry.name)
        entry.latch.countDown()
        context.getSystemService(NotificationManager::class.java).cancel(entry.notificationId)
    }

    private fun publish() {
        CompanionState.setApprovals(pending.values.map { CompanionState.Approval(it.clientId, it.name) })
    }

    private fun showNotification(context: Context, entry: Pending) {
        fun decision(allow: Boolean): PendingIntent {
            val intent = Intent(context, ApprovalReceiver::class.java)
                .setAction(ACTION_DECIDE)
                .putExtra(EXTRA_CLIENT, entry.clientId)
                .putExtra(EXTRA_ALLOW, allow)
            return PendingIntent.getBroadcast(context, entry.notificationId * 2 + if (allow) 1 else 0, intent,
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        }
        val open = PendingIntent.getActivity(context, entry.notificationId,
            Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val notification = Notifications.builder(context, Notifications.CHANNEL_REQUESTS)
            .setContentTitle("Allow ${entry.name}?")
            .setContentText("This computer wants to access files on this phone.")
            .setCategory(android.app.Notification.CATEGORY_CALL)
            .setContentIntent(open)
            .setFullScreenIntent(open, true)
            .setAutoCancel(true)
            .setTimeoutAfter(TIMEOUT_SECONDS * 1000)
            .addAction(android.app.Notification.Action.Builder(null, "Deny", decision(false)).build())
            .addAction(android.app.Notification.Action.Builder(null, "Allow", decision(true)).build())
            .build()
        runCatching { context.getSystemService(NotificationManager::class.java).notify(entry.notificationId, notification) }
    }
}

class ApprovalReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Approvals.ACTION_DECIDE) return
        val client = intent.getStringExtra(Approvals.EXTRA_CLIENT) ?: return
        Approvals.resolve(context, client, intent.getBooleanExtra(Approvals.EXTRA_ALLOW, false))
    }
}
