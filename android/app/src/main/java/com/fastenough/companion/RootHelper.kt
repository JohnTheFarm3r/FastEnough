package com.fastenough.companion

import android.content.Context
import android.util.Log
import java.io.File
import kotlin.concurrent.thread

/** Starts the bundled server binary as root through su. */
object RootHelper {
    private const val TAG = "FastEnoughRoot"
    private var process: Process? = null

    @Synchronized
    fun launch(context: Context, name: String, secret: String): Boolean {
        stop()
        val binary = File(context.applicationInfo.nativeLibraryDir, "libafmserver.so")
        if (!binary.exists()) {
            Log.w(TAG, "Root helper binary is missing")
            return false
        }
        val command = "'${binary.absolutePath}' --root-helper $name $secret"
        return try {
            // The global mount namespace shows other apps' data, like a root ADB shell.
            val args = if (supportsMountMaster()) listOf("su", "--mount-master", "-c", command) else listOf("su", "-c", command)
            val started = ProcessBuilder(args).redirectErrorStream(true).start()
            process = started
            CompanionState.update { it.copy(rootActive = true) }
            thread(name = "root-helper-log", isDaemon = true) {
                runCatching { started.inputStream.bufferedReader().forEachLine { Log.i(TAG, it) } }
                val code = runCatching { started.waitFor() }.getOrDefault(-1)
                Log.i(TAG, "Root helper exited ($code)")
                synchronized(this) {
                    // Only the current helper may cancel a pending root request.
                    if (process === started) {
                        process = null
                        NativeBridge.nativeRootHelperExited()
                    }
                }
                CompanionState.update { it.copy(rootActive = false) }
            }
            true
        } catch (error: Exception) {
            Log.w(TAG, "su is not available: ${error.message}")
            false
        }
    }

    private var mountMaster: Boolean? = null

    /** Magisk and KernelSU accept --mount-master. Asking for help does not request root. */
    private fun supportsMountMaster(): Boolean {
        mountMaster?.let { return it }
        val supported = runCatching {
            val help = ProcessBuilder("su", "--help").redirectErrorStream(true).start()
            val text = help.inputStream.bufferedReader().readText()
            help.waitFor()
            text.contains("mount-master")
        }.getOrDefault(false)
        mountMaster = supported
        return supported
    }

    @Synchronized
    fun stop() {
        process?.destroy()
        process = null
    }
}
