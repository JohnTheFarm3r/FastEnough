package com.fastenough.companion

import android.app.Application
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

class CompanionApp : Application() {
    override fun onCreate() {
        super.onCreate()
        CompanionSettings.init(this)
        Notifications.createChannels(this)
        CompanionState.refreshTrusted()
    }
}

/** Starts the file server after the phone boots or the app is updated. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        CompanionSettings.init(context)
        val boot = intent.action == Intent.ACTION_BOOT_COMPLETED
        val updated = intent.action == Intent.ACTION_MY_PACKAGE_REPLACED
        if ((boot && CompanionSettings.startOnBoot) || (updated && CompanionSettings.serverEnabled)) {
            if (CompanionSettings.serverEnabled) runCatching { CompanionService.start(context) }
        }
    }
}
