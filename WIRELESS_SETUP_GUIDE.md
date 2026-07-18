# E25 Press Monitor - OTA & Wireless Monitoring Setup Guide

## 🎯 Overview
This guide will help you set up Over-The-Air (OTA) updates and wireless serial monitoring for your ESP32 production counter, so you can:
- Monitor cycle validation and timestamps remotely
- Update firmware without cables
- Debug issues in real-time

## 📋 Step-by-Step Setup Procedure

### Phase 1: Initial Cable Upload (One-Time Only)

1. **Connect ESP32 to Computer**
   - Connect ESP32 to your computer via USB cable
   - Make sure ESP32 drivers are installed

2. **Find Your COM Port**
   - Open Windows Device Manager → Ports (COM & LPT)
   - Look for "Silicon Labs CP210x" or "CH340" or similar
   - Note the COM port number (e.g., COM3, COM4, COM7, etc.)

3. **Update platformio.ini**
   - Open: `c:\Users\Deshan\Documents\PlatformIO\Projects\E-25_Press\platformio.ini`
   - Change line: `upload_port = COM5` to your actual port
   - Example: `upload_port = COM3`

4. **Upload Initial Firmware**
   ```powershell
   cd "c:\Users\Deshan\Documents\PlatformIO\Projects\E-25_Press"
   C:\Users\Deshan\.platformio\penv\Scripts\platformio.exe run --target upload
   ```

5. **Verify Upload Success**
   - Look for "SUCCESS" message
   - ESP32 should connect to WiFi and show IP address in serial monitor

### Phase 2: Switch to Wireless Mode

6. **Find ESP32's IP Address**
   - Check serial monitor output for: "IP address: 192.168.x.x"
   - OR check your router's connected devices for "E25-Press-Monitor"
   - Write down this IP address

7. **Disconnect USB Cable**
   - ESP32 should remain powered by the machine
   - WiFi connection should stay active

8. **Update platformio.ini for OTA**
   ```ini
   [env:esp32dev]
   platform = espressif32@6.8.1
   board = esp32dev
   framework = arduino
   monitor_speed = 115200
   upload_speed = 460800
   ; upload_port = COM5  ; Comment out cable upload
   
   lib_deps =
       knolleary/PubSubClient @ ^2.8
       bblanchon/ArduinoJson @ ^6.21.3
   
   build_flags = 
       -D MQTT_MAX_PACKET_SIZE=1024
   
   # Enable OTA uploads
   upload_protocol = espota
   upload_port = 192.168.1.100  ; Use your ESP32's IP address
   ```

### Phase 3: Wireless Serial Monitoring

9. **Monitor Serial Output Wirelessly**
   
   **Option A: Using Telnet (Recommended)**
   ```powershell
   # Replace with your ESP32's IP address
   telnet 192.168.1.100
   ```
   
   **Option B: Using PuTTY**
   - Download PuTTY
   - Connection Type: Telnet
   - Host Name: Your ESP32's IP address
   - Port: 23
   - Click "Open"

   **Option C: Using Windows Terminal**
   ```powershell
   # Install telnet client if not available
   dism /online /Enable-Feature /FeatureName:TelnetClient
   
   # Connect to ESP32
   telnet 192.168.1.100
   ```

### Phase 4: OTA Firmware Updates

10. **Upload New Firmware Wirelessly**
    ```powershell
    cd "c:\Users\Deshan\Documents\PlatformIO\Projects\E-25_Press"
    C:\Users\Deshan\.platformio\penv\Scripts\platformio.exe run --target upload
    ```
    
    - You'll see "OTA Update starting" messages
    - Progress percentage will be shown
    - ESP32 will reboot with new firmware

## 🔧 Troubleshooting

### OTA Upload Issues
- **"No OTA port found"**: Check ESP32 is powered and connected to WiFi
- **"Authentication failed"**: Verify the `OTA_PASSWORD` environment variable matches `OTA_PASSWORD` in `include/secrets.h`
- **"Connection timeout"**: Check firewall settings, try IP instead of hostname

### Wireless Monitoring Issues
- **Can't connect via telnet**: Check ESP32 IP address, ensure port 23 is open
- **Connection drops**: ESP32 may have rebooted, reconnect
- **No output**: Serial may be busy, try disconnecting and reconnecting

### Network Issues
- **ESP32 not found**: Check router DHCP lease table
- **IP changed**: ESP32 got new IP from router, update platformio.ini

## 📊 What You'll See in Wireless Monitor

When connected via telnet, you'll see real-time output:
```
🔗 E25 Press Monitor - Serial Console Connected
📊 Current Status: Day=25, Night=18, Total=43
📋 Monitoring cycle validation and timestamps...

=== PROCESSING BUTTON PRESS ===
Current time: 14:32:15 | Current shift: Day
=== CYCLE TIME VALIDATION ===
Previous timestamp: 1234567
Current timestamp: 1714567
Time difference: 480000 ms (480.0 seconds)
Valid range: 480000 ms to 750000 ms
✓ Valid cycle time - proceeding with count
DAY SHIFT - Calculation: 1 press * 4 moulds = 4 added to day_count
After: day_count=29, night_count=18, total_count=47
```

## 🎛️ Available Commands

Once connected wirelessly, you can:
- **See real-time button press validation**
- **Monitor timestamp differences**  
- **Check cycle time calculations**
- **View daily reset activities at 6 AM**
- **Debug ThingsBoard communication**

## 🚀 Benefits

✅ **Remote Monitoring**: Debug from your office, no need to go to machine
✅ **Wireless Updates**: Push firmware fixes without interrupting production
✅ **Real-time Debugging**: See exactly what's happening with cycle validation
✅ **No Cables**: Complete wireless operation after initial setup

## 🔒 Security Note

- OTA password: set via `OTA_PASSWORD` in `include/secrets.h` — keep it independent of the Wi-Fi password
- Telnet is unencrypted - use only on trusted networks
- Change passwords in production environment

## 📞 Support

If you encounter issues:
1. Check ESP32 is powered and WiFi connected
2. Verify IP address hasn't changed
3. Try power cycling the ESP32
4. Check router firewall settings for ports 23 and 3232
