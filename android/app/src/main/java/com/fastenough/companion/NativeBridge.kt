package com.fastenough.companion

/** Connects the Kotlin service to the native file server. */
object NativeBridge {
    interface Host {
        fun authorize(request: String): String
        fun appCommand(command: Int, payload: String): String
        fun launchRootHelper(name: String, secret: String): Boolean
        fun clientsChanged(count: Int)
    }

    @Volatile var host: Host? = null
    private var initialized = false

    init {
        System.loadLibrary("afmcompanion")
    }

    @Synchronized
    fun ensureInitialized() {
        if (!initialized) {
            nativeInit()
            initialized = true
        }
    }

    // Called from native threads.
    fun authorize(request: String): String = host?.authorize(request) ?: "NThe companion app is not ready"
    fun appCommand(command: Int, payload: String): String = host?.appCommand(command, payload) ?: "NThe companion app is not ready"
    fun launchRootHelper(name: String, secret: String): Boolean = host?.launchRootHelper(name, secret) ?: false
    fun clientsChanged(count: Int) { host?.clientsChanged(count) }

    external fun nativeInit()
    external fun nativeStartServer(port: Int): Int
    external fun nativeStopServer()
    external fun nativeStopRoot()
    external fun nativeRootHelperExited()
}
