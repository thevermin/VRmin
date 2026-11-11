VRmin for Windows (beta)
========================

Play the Steam VR games on your Windows PC on the Quest, and use the Windows desktop from the headset.

What you need
- 64-bit Windows 10 or 11.
- An NVIDIA, AMD or Intel graphics card (the PC uses it to compress the picture).
- The Quest and the PC on the same Wi-Fi network (better if the PC is wired to the router).
- The VRmin app on the Quest (the same one used with the Mac).

How to use it
1. Unzip this folder anywhere (for example in Documents) and open "VRmin Host.exe". Its window shows the connection,
   your Steam games and the settings (it uses WebView2, which comes with Windows 10 and 11).
   If Windows says the app isn't recognized: "More info" › "Run anyway".
2. When Windows Firewall asks, allow access on private networks.
3. Open VRmin on the Quest. The first time, type the code shown in the PC window into the headset.
4. Pick a game in the headset. The first time, Windows asks for administrator permission: it's so that VR games
   use VRmin instead of SteamVR.

While the VRmin Host window is open, SteamVR games use VRmin. When you close it, SteamVR is back as it was.
The "Back to SteamVR" button undoes everything VRmin changed in Windows.

Quality
- Pick Low, Medium, High or Ultra in the headset (Settings, or the in-game menu). The one for your graphics card is
  marked as recommended. Each game can also have its own quality.

Headset microphone in games
- Games normally use the PC's microphone. To use the Quest's microphone instead, install VB-CABLE (free,
  https://vb-audio.com/Cable/), then click "Use the headset's microphone" in VRmin Host.
- While you play, Windows' microphone becomes "CABLE Output" (the headset's microphone). When you stop playing, the
  microphone you had comes back.

Virtual monitor
- One more PC screen that only shows in the headset. Windows needs a driver for that: VRmin uses Virtual Display
  Driver (free, MIT, https://github.com/VirtualDrivers/Virtual-Display-Driver). Click "Install" in "Virtual monitor"
  once (Windows asks for administrator permission). Then open it from the headset with "Add monitor".
- VRmin only adds it to the desktop while the headset shows it. To remove the driver: Device Manager, Display
  adapters, Virtual Display Driver, Uninstall device.

SteamVR games use OpenComposite, built by VRmin and included in this folder (OpenComposite, GPL 3).
3D with AI uses ONNX Runtime (MIT) and DirectML (Microsoft), in the "ai" folder, and downloads the Depth Anything V2
small model (Apache 2.0) the first time. Licenses are in the "licenses" folder.

If something goes wrong: click "Something's wrong". It saves a report on the Desktop and opens the GitHub page to
send it in an Issue.
https://github.com/thevermin/VRmin/issues

Not on Windows yet: following your head with the camera in Sentinel Mode.
