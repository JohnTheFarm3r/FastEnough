package com.fastenough.companion

import android.Manifest
import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.text.DateFormat
import java.util.Date

class MainActivity : ComponentActivity() {
    companion object {
        @Volatile var visible = false
    }

    private var refreshKey by mutableIntStateOf(0)
    private val notificationPermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { refreshKey++ }
    private val storagePermission =
        registerForActivityResult(ActivityResultContracts.RequestPermission()) { refreshKey++ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        CompanionSettings.init(this)
        CompanionState.refreshTrusted()
        if (CompanionSettings.serverEnabled) CompanionService.start(this)
        if (Build.VERSION.SDK_INT >= 33 && !Permissions.hasNotificationAccess(this))
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        setContent { CompanionTheme { Screen() } }
    }

    override fun onResume() {
        super.onResume()
        visible = true
        refreshKey++
    }

    override fun onPause() {
        visible = false
        super.onPause()
    }

    private fun launch(vararg intents: Intent) {
        for (intent in intents) {
            if (runCatching { startActivity(intent) }.isSuccess) return
        }
    }

    private fun requestStorage() {
        if (Build.VERSION.SDK_INT >= 30) launch(Permissions.storageSettingsIntent(this))
        else storagePermission.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE)
    }

    @Composable
    private fun Screen() {
        val status by CompanionState.status.collectAsState()
        val approvals by CompanionState.approvals.collectAsState()
        val trusted by CompanionState.trusted.collectAsState()
        @Suppress("UNUSED_VARIABLE") val refresh = refreshKey
        var enabled by remember { mutableStateOf(CompanionSettings.serverEnabled) }
        var startOnBoot by remember { mutableStateOf(CompanionSettings.startOnBoot) }
        var allowRoot by remember { mutableStateOf(CompanionSettings.allowRoot) }

        Column(
            Modifier
                .fillMaxSize()
                .background(Palette.background)
                .safeDrawingPadding()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Header(status, enabled) { on ->
                enabled = on
                CompanionSettings.serverEnabled = on
                if (on) CompanionService.start(this@MainActivity) else CompanionService.stop(this@MainActivity)
            }

            val storage = Permissions.hasStorageAccess(this@MainActivity)
            val notifications = Permissions.hasNotificationAccess(this@MainActivity)
            val battery = Permissions.ignoresBatteryOptimizations(this@MainActivity)
            val installs = AppCommands.hasUnknownSourcesAccess(this@MainActivity)
            Section("Permissions") {
                PermissionRow("All files access", "Required to browse and transfer your files.", storage) { requestStorage() }
                PermissionRow("Notifications", "Shows connection requests from computers.", notifications) {
                    if (Build.VERSION.SDK_INT >= 33) notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
                }
                PermissionRow("Run in background", "Keeps transfers going when the screen is off.", battery) {
                    launch(Permissions.batteryIntent(this@MainActivity))
                }
                PermissionRow("Install apps", "Optional. Lets your computer install APK files.", installs) {
                    launch(Permissions.unknownSourcesIntent(this@MainActivity))
                }
            }

            Section("Connect from your computer") {
                Text(CompanionSettings.deviceName(this@MainActivity), color = Palette.text, fontWeight = FontWeight.SemiBold, fontSize = 16.sp)
                Spacer(Modifier.height(6.dp))
                val wifi = status.addresses.filter { it.kind == "wifi" || it.kind == "ethernet" }
                val usb = status.addresses.filter { it.kind == "usb" }
                LinkRow("WiFi", wifi.joinToString { it.ip }.ifEmpty { "Not connected" }, wifi.isNotEmpty())
                LinkRow("USB tethering", usb.joinToString { it.ip }.ifEmpty { "Off" }, usb.isNotEmpty())
                if (status.rootActive) LinkRow("Root access", "Active", true)
                Spacer(Modifier.height(8.dp))
                Hint("Open Fast Enough on your computer. This phone appears on the Connections page when both devices share a WiFi network or when USB tethering is on.")
                Spacer(Modifier.height(8.dp))
                OutlinedButton(onClick = { launch(*Permissions.tetheringIntents().toTypedArray()) }) {
                    Text("Open USB tethering settings")
                }
            }

            Section("Trusted computers") {
                if (trusted.isEmpty()) Hint("No computers yet. You will be asked to allow a computer the first time it connects.")
                trusted.forEach { computer ->
                    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Column(Modifier.weight(1f)) {
                            Text(computer.name, color = Palette.text, fontWeight = FontWeight.Medium)
                            Text("Last used " + DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(computer.lastSeen)),
                                color = Palette.muted, fontSize = 12.sp)
                        }
                        TextButton(onClick = { CompanionSettings.forget(computer.id) }) { Text("Remove", color = Palette.red) }
                    }
                }
            }

            Section("Settings") {
                ToggleRow("Start when the phone turns on", startOnBoot) { startOnBoot = it; CompanionSettings.startOnBoot = it }
                ToggleRow("Allow root access when requested", allowRoot) {
                    allowRoot = it
                    CompanionSettings.allowRoot = it
                    if (!it) { NativeBridge.nativeStopRoot(); RootHelper.stop() }
                }
                Hint("Root access is only used when you turn on Root for this phone in Fast Enough, and your root manager asks you to confirm it.")
            }
        }

        approvals.firstOrNull()?.let { request ->
            AlertDialog(
                onDismissRequest = {},
                containerColor = Palette.surface,
                title = { Text("Allow ${request.name}?", color = Palette.text) },
                text = { Text("This computer wants to browse and transfer files on this phone. Only allow computers you trust.", color = Palette.muted) },
                confirmButton = {
                    Button(onClick = { Approvals.resolve(this@MainActivity, request.clientId, true) }) { Text("Allow") }
                },
                dismissButton = {
                    TextButton(onClick = { Approvals.resolve(this@MainActivity, request.clientId, false) }) { Text("Deny") }
                }
            )
        }
    }

    @Composable
    private fun Header(status: CompanionState.Status, enabled: Boolean, onToggle: (Boolean) -> Unit) {
        Card(colors = CardDefaults.cardColors(containerColor = Palette.surface), shape = RoundedCornerShape(14.dp)) {
            Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Fast Enough", color = Palette.text, fontSize = 22.sp, fontWeight = FontWeight.Bold)
                    Spacer(Modifier.height(4.dp))
                    val (label, color) = when {
                        !enabled -> "Stopped" to Palette.muted
                        status.serverError.isNotEmpty() -> status.serverError to Palette.red
                        status.clients > 0 -> "Computer connected" to Palette.green
                        status.serverRunning -> "Ready for connections" to Palette.blue
                        else -> "Starting..." to Palette.muted
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.size(8.dp).clip(CircleShape).background(color))
                        Spacer(Modifier.width(8.dp))
                        Text(label, color = color, fontSize = 14.sp)
                    }
                }
                Switch(checked = enabled, onCheckedChange = onToggle)
            }
        }
    }

    @Composable
    private fun Section(title: String, content: @Composable () -> Unit) {
        Card(colors = CardDefaults.cardColors(containerColor = Palette.surface), shape = RoundedCornerShape(14.dp)) {
            Column(Modifier.fillMaxWidth().padding(16.dp)) {
                Text(title, color = Palette.text, fontSize = 17.sp, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.height(10.dp))
                content()
            }
        }
    }

    @Composable
    private fun PermissionRow(title: String, detail: String, granted: Boolean, onGrant: () -> Unit) {
        Row(Modifier.fillMaxWidth().padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(title, color = Palette.text, fontWeight = FontWeight.Medium)
                Text(detail, color = Palette.muted, fontSize = 12.sp)
            }
            if (granted) Text("Granted", color = Palette.green, fontSize = 13.sp)
            else Button(onClick = onGrant, colors = ButtonDefaults.buttonColors(containerColor = Palette.blue)) {
                Text("Allow", color = Color.Black)
            }
        }
    }

    @Composable
    private fun LinkRow(title: String, value: String, active: Boolean) {
        Row(Modifier.fillMaxWidth().padding(vertical = 3.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(8.dp).clip(CircleShape).background(if (active) Palette.green else Palette.border))
            Spacer(Modifier.width(10.dp))
            Text(title, color = Palette.muted, modifier = Modifier.width(120.dp), fontSize = 14.sp)
            Text(value, color = Palette.text, fontSize = 14.sp)
        }
    }

    @Composable
    private fun ToggleRow(title: String, checked: Boolean, onChange: (Boolean) -> Unit) {
        Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(title, color = Palette.text, modifier = Modifier.weight(1f))
            Switch(checked = checked, onCheckedChange = onChange)
        }
    }

    @Composable
    private fun Hint(text: String) {
        Text(text, color = Palette.muted, fontSize = 13.sp, lineHeight = 18.sp)
    }
}

private object Palette {
    val background = Color(0xFF0E1116)
    val surface = Color(0xFF171B22)
    val border = Color(0xFF39404C)
    val text = Color(0xFFE8ECF2)
    val muted = Color(0xFF9AA4B2)
    val blue = Color(0xFF5BDEFF)
    val green = Color(0xFF4ADE80)
    val red = Color(0xFFFF8F97)
}

@Composable
private fun CompanionTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = darkColorScheme(
            primary = Palette.blue,
            onPrimary = Color.Black,
            surface = Palette.surface,
            background = Palette.background,
        ),
        content = content
    )
}
