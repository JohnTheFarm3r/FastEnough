Fast Enough - Android File Explorer
====================================

Getting Started
---------------
1. Run "Fast Enough - Android File Explorer.exe"
2. Connect your Android phone via USB cable
3. Enable Developer Options and USB Debugging on your phone
4. When prompted on your phone, tap Allow to authorize this computer

For wireless connections, use the Setup Wizard or WiFi ADB Pairing
from within the app.

Keep all files and folders from this ZIP together. The included
platform-tools folder contains ADB, which the app needs to find and
connect to Android devices. A separate Android SDK install is not
required when running from the release ZIP.


Without USB Debugging (Companion App)
-------------------------------------
Some apps refuse to run while Developer Options are enabled. For these
phones, install "Fast Enough Companion.apk" from this ZIP on the phone
instead of turning on USB debugging:

1. Copy the APK to the phone and open it to install
2. Open Fast Enough Companion and allow All files access
3. Keep the phone on the same WiFi as this computer, or plug it in and
   turn on USB tethering in the phone's network settings
4. Open Connections in Fast Enough, click the phone, then tap Allow on
   the phone to trust this computer

WiFi and USB tethering can be used at the same time for faster
transfers.

If a phone has USB debugging on and also runs the companion app, Fast
Enough recognizes it as one phone. Use the ADB / IP switch next to the
phone in Connections, or in its device details, to choose how it
connects. With IP selected, Fast Enough leaves USB debugging alone for
that phone and uses the companion app over USB tethering or WiFi.

Rooted phones can still use the Root button. The companion app asks
your root manager for permission the first time.


Dokan Driver (Required for Virtual Drive)
------------------------------------------
To mount your Android device as a drive letter in Windows Explorer,
install the included Dokan driver:

1. Run "Dokan_x64.msi" and follow the installer
2. Restart your computer if prompted
3. The virtual drive feature will then be available in the app


Windows Projected File System (Required)
-----------------------------------------
The app requires the Windows Projected File System feature to be
enabled. Without it, the app will not start.

To enable it:
1. Open the Start menu and search for "Turn Windows features on or off"
2. Scroll down and check "Windows Projected File System"
3. Click OK and wait for the installation to complete
4. Restart your computer if prompted

A screenshot (projfs_enable.png) is included in this ZIP showing
the correct setting to enable.
