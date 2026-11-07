package com.fastenough.companion

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Context

object Notifications {
    const val CHANNEL_SERVICE = "service"
    const val CHANNEL_REQUESTS = "requests"
    const val CHANNEL_ACTIONS = "actions"
    const val SERVICE_ID = 1

    fun createChannels(context: Context) {
        val manager = context.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel(CHANNEL_SERVICE, "File access",
            NotificationManager.IMPORTANCE_LOW).apply { setShowBadge(false) })
        manager.createNotificationChannel(NotificationChannel(CHANNEL_REQUESTS, "Connection requests",
            NotificationManager.IMPORTANCE_HIGH))
        manager.createNotificationChannel(NotificationChannel(CHANNEL_ACTIONS, "App installs and removals",
            NotificationManager.IMPORTANCE_HIGH))
    }

    fun builder(context: Context, channel: String): Notification.Builder =
        Notification.Builder(context, channel).setSmallIcon(R.drawable.ic_stat_companion)
}
