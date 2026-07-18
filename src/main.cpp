#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include "secrets.h"  // WIFI_SSID, WIFI_PASSWORD, TOKEN, OTA_PASSWORD — see include/secrets.h.example

// ==== ThingsBoard Server Details ====
#define THINGSBOARD_SERVER "demo.thingsboard.io"   // or your local IP if using Docker
#define THINGSBOARD_PORT   1883

// ==== Button Configuration ====
#define BUTTON_PIN 4  // GPIO pin for push button
#define ACCIDENTLY_PRESSED 5000  // 5 seconds - minimum time between valid button presses

// ==== Cycle Time Validation ==== 8 - 14 MIN AND 18 ABOVE
#define ATLEAST 0*1000    // Minimum additional time in milliseconds
#define ATMOST 360*1000    // Maximum additional time in milliseconds 2 minutes // E-69 - 6 minutes
#define EXCEED_LIMIT 18*60*1000 // E - 25, 12 minutes in milliseconds - for long idle periods  // E -69 - 18 minutes

WiFiClient espClient;
PubSubClient client(espClient);
Preferences preferences;
WebServer server(80);  // Web server on port 80

// Time configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;  // GMT+5:30 for Sri Lanka (adjust timezone)
const int daylightOffset_sec = 0;

// Global variables
int received_moulds_day = 0;
int received_moulds_night = 0;
int received_op_time = 0;        // Operation time in seconds - NO DEFAULT VALUE, must be set by user
bool op_time_received = false;   // Track if Op_time has been received from dashboard
String received_day_assignee = "";  // Day shift assignee name
bool day_assignee_received = false;  // Track if day_assignee has been received from dashboard
String received_specific_item = "";  // Specific item name
bool specific_item_received = false;  // Track if Specific_Item has been received from dashboard
String received_colour_code = "";  // Colour code
bool colour_code_received = false;  // Track if Colour_code has been received from dashboard
int received_target = 0;  // Production target
bool target_received = false;  // Track if target has been received from dashboard
String received_night_assignee = "";  // Night shift assignee name
bool night_assignee_received = false;  // Track if night_assignee has been received from dashboard
int received_damage = 0;  // Damage count input from dashboard
bool damage_received = false;  // Track if damage has been received from dashboard
int last_day_correction = 0;       // Last applied day correction value
int last_night_correction = 0;     // Last applied night correction value
bool day_correction_applied = false;   // Track if day correction was applied
bool night_correction_applied = false; // Track if night correction was applied
int day_count = 0;           // Day shift count (6AM-6PM)
int night_count = 0;         // Night shift count (6PM-6AM)
int total_count = 0;         // Total count (day_count + night_count)
int hourly_counts[24] = {0}; // Array to store counts for each hour (0-23)
int current_hour_count = 0;  // Current hour's production count
int last_tracked_hour = -1;  // Last hour we tracked (to detect hour changes)
bool moulds_day_received = false;
bool moulds_night_received = false;
bool last_button_state = HIGH;
unsigned long last_button_time = 0;  // Track last valid button press time (for 5-second prevention)
unsigned long previous_button_timestamp = 0;  // Track previous button press timestamp for cycle validation
unsigned long button_debounce = 200;  // 200ms debounce for industrial button
String last_reset_date = "";  // Track when last reset occurred

// Function declarations
void saveDataToNVS();
void loadDataFromNVS();
void requestSharedAttributes();
void processButtonPress();
void reconnect();
void checkDailyReset();
void checkMissedDailyReset();
void checkHourlyUpdate();
void sendHourlyData(int hour_to_send);
int getCurrentHour();
String getCurrentDate();
String getCurrentTime();
bool isTime6AM();
bool isDayShift();
int getCurrentMoulds();
bool isCurrentShiftConfigured();
String getCurrentShiftName();

// ==== Centralized Complete Telemetry Function ====
String buildCompleteTelemetryPayload(String primary_field = "", String primary_value = "") {
  String payload = "{";
  
  // Add primary field if provided (for specific events like button press or user input changes)
  if (primary_field.length() > 0 && primary_value.length() > 0) {
    payload += "\"" + primary_field + "\":" + primary_value + ",";
  }
  
  // Always include production counts
  payload += "\"day_count\":" + String(day_count) +
             ",\"night_count\":" + String(night_count) +
             ",\"total_count\":" + String(total_count);
  
  // Add current hour production count only (no hour number)
  int current_hour = getCurrentHour();
  if (current_hour != -1) {
    payload += ",\"total_for_hour\":" + String(hourly_counts[current_hour]);
  }
  
  // Always include ALL user input variables with their _dash versions (even if empty/default)
  if (moulds_day_received) {
    payload += ",\"moulds_Day_dash\":" + String(received_moulds_day);
  }
  if (moulds_night_received) {
    payload += ",\"moulds_Night_dash\":" + String(received_moulds_night);
  }
  if (day_assignee_received || received_day_assignee.length() > 0) {
    payload += ",\"day_assignee_dash\":\"" + received_day_assignee + "\"";
  }
  if (specific_item_received || received_specific_item.length() > 0) {
    payload += ",\"Specific_Item_dash\":\"" + received_specific_item + "\"";
  }
  if (colour_code_received || received_colour_code.length() > 0) {
    payload += ",\"Colour_code_dash\":\"" + received_colour_code + "\"";
  }
  if (target_received) {
    payload += ",\"target_dash\":" + String(received_target);
  }
  if (night_assignee_received || received_night_assignee.length() > 0) {
    payload += ",\"night_assignee_dash\":\"" + received_night_assignee + "\"";
  }
  
  // Always add timestamp
  String current_time = getCurrentTime();
  if (current_time.length() > 0) {
    payload += ",\"timestamp\":\"" + current_time + "\"";
  }
  
  payload += "}";
  return payload;
}

void callback(char* topic, byte* payload, unsigned int length) {
  String incoming = "";
  for (unsigned int i = 0; i < length; i++) {
    incoming += (char)payload[i];
  }

  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(incoming);

  // Handle RPC requests from ThingsBoard
  String topicStr = String(topic);
  if (topicStr.indexOf("rpc/request") != -1) {
    // Extract request ID from topic (e.g., "v1/devices/me/rpc/request/123")
    int lastSlash = topicStr.lastIndexOf("/");
    String requestId = topicStr.substring(lastSlash + 1);
    
    // Check what method is being called
    if (incoming.indexOf("checkStatus") != -1) {
      // Respond to status check with current device information
      String response_topic = "v1/devices/me/rpc/response/" + requestId;
      String response_payload = "{\"status\":\"online\"" +
                               String(",\"total_count\":") + String(total_count) + 
                               String(",\"day_count\":") + String(day_count) + 
                               String(",\"night_count\":") + String(night_count) + 
                               String(",\"current_shift\":\"") + getCurrentShiftName() + "\"" +
                               String(",\"moulds_day_received\":") + String(moulds_day_received ? "true" : "false") +
                               String(",\"moulds_night_received\":") + String(moulds_night_received ? "true" : "false") +
                               String(",\"received_moulds_day\":") + String(received_moulds_day) +
                               String(",\"received_moulds_night\":") + String(received_moulds_night) + "}";
      
      client.publish(response_topic.c_str(), response_payload.c_str());
      Serial.println("✓ Responded to status check");
      return; // Exit early since this was an RPC request
    }
    
    // Handle manual reset command from dashboard
    if (incoming.indexOf("manualReset") != -1) {
      Serial.println("🔄 MANUAL RESET COMMAND RECEIVED FROM DASHBOARD");
      Serial.print("Previous values: day_count=");
      Serial.print(day_count);
      Serial.print(", night_count=");
      Serial.print(night_count);
      Serial.print(", total_count=");
      Serial.println(total_count);
      
      // Reset ALL counters manually
      day_count = 0;
      night_count = 0;
      total_count = 0;
      current_hour_count = 0;
      // Keep tracking current hour - don't reset to -1
      
      // Reset all hourly counts
      for (int i = 0; i < 24; i++) {
        hourly_counts[i] = 0;
      }
      
      // Update reset date to today
      last_reset_date = getCurrentDate();
      last_button_time = 0;  // Allow immediate button press
      
      saveDataToNVS();
      
      Serial.print("Manual reset complete. New counts - day: ");
      Serial.print(day_count);
      Serial.print(", night: ");
      Serial.print(night_count);
      Serial.print(", total: ");
      Serial.println(total_count);
      Serial.println("✓ Ready for operations after manual reset!");
      
      // Respond to dashboard with success
      String response_topic = "v1/devices/me/rpc/response/" + requestId;
      String response_payload = "{\"success\":true" +
                               String(",\"day_count\":") + String(day_count) + 
                               String(",\"night_count\":") + String(night_count) +
                               String(",\"total_count\":") + String(total_count) + "}";
      
      client.publish(response_topic.c_str(), response_payload.c_str());
      
      // Also send telemetry data immediately with complete info
      int current_hour = getCurrentHour();
      String telemetry_payload = "{\"day_count\":" + String(day_count) + 
                                ",\"night_count\":" + String(night_count) +
                                ",\"total_count\":" + String(total_count);
      
      // Add current hour production count only after reset
      if (current_hour != -1) {
        telemetry_payload += ",\"total_for_hour\":" + String(hourly_counts[current_hour]);
      }
      
      // Add day assignee if available
      if (day_assignee_received && received_day_assignee.length() > 0) {
        telemetry_payload += ",\"day_assignee_dash\":\"" + received_day_assignee + "\"";
      }
      
      // Add timestamp
      String current_time = getCurrentTime();
      if (current_time.length() > 0) {
        telemetry_payload += ",\"timestamp\":\"" + current_time + "\"";
      }
      
      telemetry_payload += "}";
      
      client.publish("v1/devices/me/telemetry", telemetry_payload.c_str());
      Serial.print("   Telemetry JSON sent: ");
      Serial.println(telemetry_payload);
      
      Serial.println("✓ Manual reset confirmation sent to ThingsBoard");
      return; // Exit early since this was an RPC request
    }
  }

  // Look for "moulds_Day" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("moulds_Day") != -1) {
    int start = incoming.indexOf("moulds_Day") + 12; // Position after "moulds_Day":
    int end = incoming.indexOf(",", start);
    if (end == -1) end = incoming.indexOf("}", start);
    if (end == -1) end = incoming.length();
    String valueStr = incoming.substring(start, end);
    valueStr.trim();
    valueStr.replace("\"", ""); // Remove quotes if present
    int new_moulds_day = valueStr.toInt();

    Serial.print("Received moulds_Day: ");
    Serial.println(new_moulds_day);
    
    // Only send telemetry if this is a new value (to avoid unnecessary telemetry)
    if (new_moulds_day != received_moulds_day) {
      received_moulds_day = new_moulds_day;
      moulds_day_received = true;
      
      // Save the received moulds value to NVS
      saveDataToNVS();
      
      // Send complete telemetry including moulds_Day change + all production data
      String moulds_day_payload = buildCompleteTelemetryPayload("moulds_Day_dash", String(received_moulds_day));
      
      if (client.connected()) {
        bool result = client.publish("v1/devices/me/telemetry", moulds_day_payload.c_str());
        if (result) {
          Serial.println("✓ Complete telemetry sent for moulds_Day change");
          Serial.print("   JSON sent: ");
          Serial.println(moulds_day_payload);
        } else {
          Serial.println("✗ Failed to send moulds_Day telemetry");
        }
      } else {
        Serial.println("✗ Not connected - will send moulds_Day data when reconnected");
      }
    } else {
      // Same value - just update the received flag and save (no telemetry)
      received_moulds_day = new_moulds_day;
      moulds_day_received = true;
      saveDataToNVS();
      Serial.println("ℹ Day moulds unchanged - no telemetry sent");
    }
  }

  // Look for "moulds_Night" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("moulds_Night") != -1) {
    int start = incoming.indexOf("moulds_Night") + 14; // Position after "moulds_Night":
    int end = incoming.indexOf(",", start);
    if (end == -1) end = incoming.indexOf("}", start);
    if (end == -1) end = incoming.length();
    String valueStr = incoming.substring(start, end);
    valueStr.trim();
    valueStr.replace("\"", ""); // Remove quotes if present
    int new_moulds_night = valueStr.toInt();

    Serial.print("Received moulds_Night: ");
    Serial.println(new_moulds_night);
    
    // Only send telemetry if this is a new value (to avoid unnecessary telemetry)
    if (new_moulds_night != received_moulds_night) {
      received_moulds_night = new_moulds_night;
      moulds_night_received = true;
      
      // Save the received moulds value to NVS
      saveDataToNVS();
      
      // Send complete telemetry including moulds_Night change + all production data
      String moulds_night_payload = buildCompleteTelemetryPayload("moulds_Night_dash", String(received_moulds_night));
      
      if (client.connected()) {
        bool result = client.publish("v1/devices/me/telemetry", moulds_night_payload.c_str());
        if (result) {
          Serial.println("✓ Complete telemetry sent for moulds_Night change");
          Serial.print("   JSON sent: ");
          Serial.println(moulds_night_payload);
        } else {
          Serial.println("✗ Failed to send moulds_Night telemetry");
        }
      } else {
        Serial.println("✗ Not connected - will send moulds_Night data when reconnected");
      }
    } else {
      // Same value - just update the received flag and save (no telemetry)
      received_moulds_night = new_moulds_night;
      moulds_night_received = true;
      saveDataToNVS();
      Serial.println("ℹ Night moulds unchanged - no telemetry sent");
    }
  }

  // Look for "Op_time" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("Op_time") != -1) {
    // Find the colon after "Op_time"
    int colonPos = incoming.indexOf(":", incoming.indexOf("Op_time"));
    if (colonPos != -1) {
      int start = colonPos + 1; // Position after the colon
      int end = incoming.indexOf(",", start);
      if (end == -1) end = incoming.indexOf("}", start);
      if (end == -1) end = incoming.length();
      
      String valueStr = incoming.substring(start, end);
      valueStr.trim();
      valueStr.replace("\"", ""); // Remove quotes if present
      int new_op_time = valueStr.toInt();

      Serial.print("Received Op_time: ");
      Serial.print(new_op_time);
      Serial.println(" seconds");
      
      // Validate Op_time is positive
      if (new_op_time > 0) {
        received_op_time = new_op_time;
        op_time_received = true;
        
        Serial.println("✓ Op_time updated successfully");
      } else {
        Serial.println("❌ Invalid Op_time value - must be greater than 0");
        Serial.println("   Op_time not updated");
        return; // Don't save invalid values
      }
      
      // Calculate cycle time limits
      unsigned long min_cycle_time = ATLEAST + (received_op_time * 1000);  // Convert seconds to milliseconds
      unsigned long max_cycle_time = ATMOST + (received_op_time * 1000);   // Convert seconds to milliseconds
      
      Serial.println("=== CYCLE TIME VALIDATION UPDATED ===");
      Serial.print("Op_time: ");
      Serial.print(received_op_time);
      Serial.println(" seconds");
      Serial.print("Minimum valid cycle time: ");
      Serial.print(min_cycle_time);
      Serial.println(" ms");
      Serial.print("Maximum valid cycle time: ");
      Serial.print(max_cycle_time);
      Serial.println(" ms");
      Serial.println("====================================");
      
      // Save the received Op_time value to NVS
      saveDataToNVS();
    } else {
      Serial.println("✗ Error parsing Op_time - colon not found");
    }
  }

  // Look for "day_assignee" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("day_assignee") != -1) {
    Serial.println("=== DAY ASSIGNEE PROCESSING ===");
    Serial.print("Full incoming message: ");
    Serial.println(incoming);
    
    // Find the colon after "day_assignee"
    int colonPos = incoming.indexOf(":", incoming.indexOf("day_assignee"));
    if (colonPos != -1) {
      int start = colonPos + 1; // Position after the colon
      int end = incoming.indexOf(",", start);
      if (end == -1) end = incoming.indexOf("}", start);
      if (end == -1) end = incoming.length();
      
      String valueStr = incoming.substring(start, end);
      valueStr.trim();
      valueStr.replace("\"", ""); // Remove quotes if present
      
      Serial.print("Parsed assignee value: '");
      Serial.print(valueStr);
      Serial.print("' | Current stored value: '");
      Serial.print(received_day_assignee);
      Serial.println("'");
      
      // Only process if this is a new assignee value
      if (valueStr != received_day_assignee) {
        Serial.println("✓ NEW ASSIGNEE DETECTED - Processing change...");
        received_day_assignee = valueStr;
        day_assignee_received = true;

        Serial.print("Received day_assignee: ");
        Serial.println(received_day_assignee);
        
        // Save the received day_assignee value to NVS
        saveDataToNVS();
        
        // Send complete telemetry including assignee change + all production data
        String assignee_payload = buildCompleteTelemetryPayload("day_assignee_dash", "\"" + received_day_assignee + "\"");
        
        if (client.connected()) {
          bool result = client.publish("v1/devices/me/telemetry", assignee_payload.c_str());
          if (result) {
            Serial.println("✓ Complete telemetry sent for day_assignee change");
            Serial.print("   JSON sent: ");
            Serial.println(assignee_payload);
          } else {
            Serial.println("✗ Failed to send day assignee telemetry");
          }
        } else {
          Serial.println("✗ Not connected - will send assignee data when reconnected");
        }
      } else {
        Serial.println("ℹ Day assignee unchanged - no telemetry sent");
      }
    } else {
      Serial.println("✗ Error parsing day_assignee - colon not found");
    }
    Serial.println("===============================");
  }

  // Look for "Specific_Item" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("Specific_Item") != -1) {
    Serial.println("=== SPECIFIC ITEM PROCESSING ===");
    Serial.print("Full incoming message: ");
    Serial.println(incoming);
    
    // Find the colon after "Specific_Item"
    int colonPos = incoming.indexOf(":", incoming.indexOf("Specific_Item"));
    if (colonPos != -1) {
      int start = colonPos + 1; // Position after the colon
      int end = incoming.indexOf(",", start);
      if (end == -1) end = incoming.indexOf("}", start);
      if (end == -1) end = incoming.length();
      
      String valueStr = incoming.substring(start, end);
      valueStr.trim();
      valueStr.replace("\"", ""); // Remove quotes if present
      
      Serial.print("Parsed specific item value: '");
      Serial.print(valueStr);
      Serial.print("' | Current stored value: '");
      Serial.print(received_specific_item);
      Serial.println("'");
      
      // Only process if this is a new specific item value
      if (valueStr != received_specific_item) {
        Serial.println("✓ NEW SPECIFIC ITEM DETECTED - Processing change...");
        received_specific_item = valueStr;
        specific_item_received = true;

        Serial.print("Received Specific_Item: ");
        Serial.println(received_specific_item);
        
        // Save the received Specific_Item value to NVS
        saveDataToNVS();
        
        // Send complete telemetry including specific item change + all production data
        String item_payload = buildCompleteTelemetryPayload("Specific_Item_dash", "\"" + received_specific_item + "\"");
        
        if (client.connected()) {
          bool result = client.publish("v1/devices/me/telemetry", item_payload.c_str());
          if (result) {
            Serial.println("✓ Complete telemetry sent for Specific_Item change");
            Serial.print("   JSON sent: ");
            Serial.println(item_payload);
          } else {
            Serial.println("✗ Failed to send specific item telemetry");
          }
        } else {
          Serial.println("✗ Not connected - will send specific item data when reconnected");
        }
      } else {
        Serial.println("ℹ Specific Item unchanged - no telemetry sent");
      }
    } else {
      Serial.println("✗ Error parsing Specific_Item - colon not found");
    }
    Serial.println("===============================");
  }

  // Look for "Colour_code" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("Colour_code") != -1) {
    Serial.println("=== COLOUR CODE PROCESSING ===");
    Serial.print("Full incoming message: ");
    Serial.println(incoming);
    
    // Find the colon after "Colour_code"
    int colonPos = incoming.indexOf(":", incoming.indexOf("Colour_code"));
    if (colonPos != -1) {
      int start = colonPos + 1; // Position after the colon
      int end = incoming.indexOf(",", start);
      if (end == -1) end = incoming.indexOf("}", start);
      if (end == -1) end = incoming.length();
      
      String valueStr = incoming.substring(start, end);
      valueStr.trim();
      valueStr.replace("\"", ""); // Remove quotes if present
      
      Serial.print("Parsed colour code value: '");
      Serial.print(valueStr);
      Serial.print("' | Current stored value: '");
      Serial.print(received_colour_code);
      Serial.println("'");
      
      // Only process if this is a new colour code value
      if (valueStr != received_colour_code) {
        Serial.println("✓ NEW COLOUR CODE DETECTED - Processing change...");
        received_colour_code = valueStr;
        colour_code_received = true;

        Serial.print("Received Colour_code: ");
        Serial.println(received_colour_code);
        
        // Save the received Colour_code value to NVS
        saveDataToNVS();
        
        // Send complete telemetry including colour code change + all production data
        String colour_payload = buildCompleteTelemetryPayload("Colour_code_dash", "\"" + received_colour_code + "\"");
        
        if (client.connected()) {
          bool result = client.publish("v1/devices/me/telemetry", colour_payload.c_str());
          if (result) {
            Serial.println("✓ Complete telemetry sent for Colour_code change");
            Serial.print("   JSON sent: ");
            Serial.println(colour_payload);
          } else {
            Serial.println("✗ Failed to send colour code telemetry");
          }
        } else {
          Serial.println("✗ Not connected - will send colour code data when reconnected");
        }
      } else {
        Serial.println("ℹ Colour Code unchanged - no telemetry sent");
      }
    } else {
      Serial.println("✗ Error parsing Colour_code - colon not found");
    }
    Serial.println("===============================");
  }

  // Look for "target" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("target") != -1) {
    Serial.println("=== TARGET PROCESSING ===");
    Serial.print("Full incoming message: ");
    Serial.println(incoming);
    
    // Find the colon after "target"
    int colonPos = incoming.indexOf(":", incoming.indexOf("target"));
    if (colonPos != -1) {
      int start = colonPos + 1; // Position after the colon
      int end = incoming.indexOf(",", start);
      if (end == -1) end = incoming.indexOf("}", start);
      if (end == -1) end = incoming.length();
      
      String valueStr = incoming.substring(start, end);
      valueStr.trim();
      valueStr.replace("\"", ""); // Remove quotes if present
      int new_target = valueStr.toInt();
      
      Serial.print("Parsed target value: ");
      Serial.print(new_target);
      Serial.print(" | Current stored value: ");
      Serial.println(received_target);
      
      // Only process if this is a new target value
      if (new_target != received_target) {
        Serial.println("✓ NEW TARGET DETECTED - Processing change...");
        received_target = new_target;
        target_received = true;

        Serial.print("Received target: ");
        Serial.println(received_target);
        
        // Save the received target value to NVS
        saveDataToNVS();
        
        // Send complete telemetry including target change + all production data
        String target_payload = buildCompleteTelemetryPayload("target_dash", String(received_target));
        
        if (client.connected()) {
          bool result = client.publish("v1/devices/me/telemetry", target_payload.c_str());
          if (result) {
            Serial.println("✓ Complete telemetry sent for target change");
            Serial.print("   JSON sent: ");
            Serial.println(target_payload);
          } else {
            Serial.println("✗ Failed to send target telemetry");
          }
        } else {
          Serial.println("✗ Not connected - will send target data when reconnected");
        }
      } else {
        Serial.println("ℹ Target unchanged - no telemetry sent");
      }
    } else {
      Serial.println("✗ Error parsing target - colon not found");
    }
    Serial.println("===============================");
  }

  // Look for "night_assignee" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("night_assignee") != -1) {
    Serial.println("=== NIGHT ASSIGNEE PROCESSING ===");
    Serial.print("Full incoming message: ");
    Serial.println(incoming);
    
    // Find the colon after "night_assignee"
    int colonPos = incoming.indexOf(":", incoming.indexOf("night_assignee"));
    if (colonPos != -1) {
      int start = colonPos + 1; // Position after the colon
      int end = incoming.indexOf(",", start);
      if (end == -1) end = incoming.indexOf("}", start);
      if (end == -1) end = incoming.length();
      
      String valueStr = incoming.substring(start, end);
      valueStr.trim();
      valueStr.replace("\"", ""); // Remove quotes if present
      
      Serial.print("Parsed night assignee value: '");
      Serial.print(valueStr);
      Serial.print("' | Current stored value: '");
      Serial.print(received_night_assignee);
      Serial.println("'");
      
      // Only process if this is a new night assignee value
      if (valueStr != received_night_assignee) {
        Serial.println("✓ NEW NIGHT ASSIGNEE DETECTED - Processing change...");
        received_night_assignee = valueStr;
        night_assignee_received = true;

        Serial.print("Received night_assignee: ");
        Serial.println(received_night_assignee);
        
        // Save the received night_assignee value to NVS
        saveDataToNVS();
        
        // Send complete telemetry including night assignee change + all production data
        String night_assignee_payload = buildCompleteTelemetryPayload("night_assignee_dash", "\"" + received_night_assignee + "\"");
        
        if (client.connected()) {
          bool result = client.publish("v1/devices/me/telemetry", night_assignee_payload.c_str());
          if (result) {
            Serial.println("✓ Complete telemetry sent for night_assignee change");
            Serial.print("   JSON sent: ");
            Serial.println(night_assignee_payload);
          } else {
            Serial.println("✗ Failed to send night assignee telemetry");
          }
        } else {
          Serial.println("✗ Not connected - will send night assignee data when reconnected");
        }
      } else {
        Serial.println("ℹ Night Assignee unchanged - no telemetry sent");
      }
    } else {
      Serial.println("✗ Error parsing night_assignee - colon not found");
    }
    Serial.println("===============================");
  }

  // Look for "damage" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("damage") != -1) {
    Serial.println("=== DAMAGE PROCESSING ===");
    Serial.print("Full incoming message: ");
    Serial.println(incoming);
    
    // Find the colon after "damage"
    int colonPos = incoming.indexOf(":", incoming.indexOf("damage"));
    if (colonPos != -1) {
      int start = colonPos + 1; // Position after the colon
      int end = incoming.indexOf(",", start);
      if (end == -1) end = incoming.indexOf("}", start);
      if (end == -1) end = incoming.length();
      
      String valueStr = incoming.substring(start, end);
      valueStr.trim();
      valueStr.replace("\"", ""); // Remove quotes if present
      int new_damage = valueStr.toInt();

      Serial.print("Received damage: ");
      Serial.println(new_damage);
      
      // Always process damage input (even if same value, as it represents new damage)
      if (new_damage > 0) {
        received_damage = new_damage;
        damage_received = true;
        
        Serial.println("🔥 DAMAGE DETECTED - Processing damage counts");
        Serial.print("   Damage amount: ");
        Serial.println(new_damage);
        
        // Determine which shift to subtract from based on current time
        bool current_is_day = isDayShift();
        String current_shift = current_is_day ? "Day" : "Night";
        
        Serial.print("   Current shift: ");
        Serial.print(current_shift);
        Serial.print(" (");
        Serial.print(getCurrentTime());
        Serial.println(")");
        
        // Store original counts for logging
        int original_day = day_count;
        int original_night = night_count;
        int original_total = total_count;
        
        // Subtract damage from appropriate shift
        if (current_is_day) {
          // Day shift - subtract from day_count
          day_count = max(0, day_count - new_damage);  // Ensure we don't go negative
          Serial.print("   Subtracting from DAY count: ");
          Serial.print(original_day);
          Serial.print(" - ");
          Serial.print(new_damage);
          Serial.print(" = ");
          Serial.println(day_count);
        } else {
          // Night shift - subtract from night_count
          night_count = max(0, night_count - new_damage);  // Ensure we don't go negative
          Serial.print("   Subtracting from NIGHT count: ");
          Serial.print(original_night);
          Serial.print(" - ");
          Serial.print(new_damage);
          Serial.print(" = ");
          Serial.println(night_count);
        }
        
        // Update total_count = day_count + night_count
        total_count = day_count + night_count;
        
        Serial.println("📊 DAMAGE SUMMARY:");
        Serial.print("   Day count: ");
        Serial.print(original_day);
        Serial.print(" → ");
        Serial.println(day_count);
        Serial.print("   Night count: ");
        Serial.print(original_night);
        Serial.print(" → ");
        Serial.println(night_count);
        Serial.print("   Total count: ");
        Serial.print(original_total);
        Serial.print(" → ");
        Serial.println(total_count);
        
        // Save to NVS immediately
        saveDataToNVS();
        
        // Send telemetry with damage_dash for timeseries tracking
        String payload = buildCompleteTelemetryPayload("damage_dash", String(new_damage));
        
        if (client.connected()) {
          bool result = client.publish("v1/devices/me/telemetry", payload.c_str());
          if (result) {
            Serial.println("✅ DAMAGE telemetry sent successfully");
            Serial.print("   JSON sent: ");
            Serial.println(payload);
            Serial.println("   📈 Damage tracked in timeseries with updated counts");
          } else {
            Serial.println("✗ Failed to send damage telemetry");
          }
        } else {
          Serial.println("✗ Not connected - will send damage data when reconnected");
        }
        
        // Reset damage input after processing
        received_damage = 0;
        
      } else {
        Serial.println("⚠ Invalid damage value (must be > 0) - ignoring");
      }
    } else {
      Serial.println("✗ Error parsing damage - colon not found");
    }
    Serial.println("===============================");
  }

  // Look for "day_correction" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("day_correction") != -1) {
    // Find the colon after "day_correction"
    int colonPos = incoming.indexOf(":", incoming.indexOf("day_correction"));
    if (colonPos != -1) {
      int start = colonPos + 1; // Position after the colon
      int end = incoming.indexOf(",", start);
      if (end == -1) end = incoming.indexOf("}", start);
      if (end == -1) end = incoming.length();
      
      String valueStr = incoming.substring(start, end);
      valueStr.trim();
      valueStr.replace("\"", ""); // Remove quotes if present
      int correction_value = valueStr.toInt();

      Serial.print("Received day_correction: ");
      Serial.println(correction_value);
      
      // Only apply correction if value is not zero
      if (correction_value != 0) {
        // Store correction value and mark as applied
        last_day_correction = correction_value;
        day_correction_applied = true;
        
        // Apply correction to day_count (can be positive or negative)
        int old_day_count = day_count;
        int old_total_count = total_count;
        
        day_count += correction_value;  // Add correction (handles + and - automatically)
        total_count = day_count + night_count;  // Recalculate total
        
        // Update hourly tracking if it's currently day shift
        int current_hour = getCurrentHour();
        if (isDayShift() && current_hour != -1) {
          hourly_counts[current_hour] += correction_value;
          current_hour_count += correction_value;
          Serial.print("📊 Day correction also updated hourly count for hour ");
          Serial.print(current_hour);
          Serial.print(": ");
          Serial.print(correction_value >= 0 ? "+" : "");
          Serial.print(correction_value);
          Serial.print(" (new total for hour: ");
          Serial.print(hourly_counts[current_hour]);
          Serial.println(")");
        }
        
        Serial.println("=== DAY COUNT CORRECTION APPLIED ===");
        Serial.print("Correction value: ");
        Serial.println(correction_value);
        Serial.print("Old day_count: ");
        Serial.print(old_day_count);
        Serial.print(" -> New day_count: ");
        Serial.println(day_count);
        Serial.print("Old total_count: ");
        Serial.print(old_total_count);
        Serial.print(" -> New total_count: ");
        Serial.println(total_count);
        Serial.println("==================================");
        
        // Save corrected values to NVS
        saveDataToNVS();
        
        // Send corrected values to ThingsBoard with complete data
        String correction_payload = "{\"day_count\":" + String(day_count) + 
                                   ",\"night_count\":" + String(night_count) +
                                   ",\"total_count\":" + String(total_count);
        
        // Add current hour production count only
        if (current_hour != -1) {
          correction_payload += ",\"total_for_hour\":" + String(hourly_counts[current_hour]);
        }
        
        // Add the applied day correction value back to dashboard
        correction_payload += ",\"day_correction_dash\":" + String(correction_value);
        
        // Add all user inputs if available
        if (day_assignee_received && received_day_assignee.length() > 0) {
          correction_payload += ",\"day_assignee_dash\":\"" + received_day_assignee + "\"";
        }
        if (specific_item_received && received_specific_item.length() > 0) {
          correction_payload += ",\"Specific_Item_dash\":\"" + received_specific_item + "\"";
        }
        if (colour_code_received && received_colour_code.length() > 0) {
          correction_payload += ",\"Colour_code_dash\":\"" + received_colour_code + "\"";
        }
        if (target_received) {
          correction_payload += ",\"target_dash\":" + String(received_target);
        }
        if (night_assignee_received && received_night_assignee.length() > 0) {
          correction_payload += ",\"night_assignee_dash\":\"" + received_night_assignee + "\"";
        }
        
        // Add timestamp
        String current_time = getCurrentTime();
        if (current_time.length() > 0) {
          correction_payload += ",\"timestamp\":\"" + current_time + "\"";
        }
        
        correction_payload += "}";
        
        client.publish("v1/devices/me/telemetry", correction_payload.c_str());
        Serial.println("✓ Day correction data sent to ThingsBoard");
        Serial.print("   JSON sent: ");
        Serial.println(correction_payload);
      } else {
        Serial.println("⚠ Day correction value is 0 - no correction applied");
      }
    } else {
      Serial.println("✗ Error parsing day_correction - colon not found");
    }
  }

  // Look for "night_correction" in shared attributes, RPC, or attribute responses
  if (incoming.indexOf("night_correction") != -1) {
    // Find the colon after "night_correction"
    int colonPos = incoming.indexOf(":", incoming.indexOf("night_correction"));
    if (colonPos != -1) {
      int start = colonPos + 1; // Position after the colon
      int end = incoming.indexOf(",", start);
      if (end == -1) end = incoming.indexOf("}", start);
      if (end == -1) end = incoming.length();
      
      String valueStr = incoming.substring(start, end);
      valueStr.trim();
      valueStr.replace("\"", ""); // Remove quotes if present
      int correction_value = valueStr.toInt();

      Serial.print("Received night_correction: ");
      Serial.println(correction_value);
      
      // Only apply correction if value is not zero
      if (correction_value != 0) {
        // Store correction value and mark as applied
        last_night_correction = correction_value;
        night_correction_applied = true;
        
        // Apply correction to night_count (can be positive or negative)
        int old_night_count = night_count;
        int old_total_count = total_count;
        
        night_count += correction_value;  // Add correction (handles + and - automatically)
        total_count = day_count + night_count;  // Recalculate total
        
        // Update hourly tracking if it's currently night shift
        int current_hour = getCurrentHour();
        if (!isDayShift() && current_hour != -1) {
          hourly_counts[current_hour] += correction_value;
          current_hour_count += correction_value;
          Serial.print("📊 Night correction also updated hourly count for hour ");
          Serial.print(current_hour);
          Serial.print(": ");
          Serial.print(correction_value >= 0 ? "+" : "");
          Serial.print(correction_value);
          Serial.print(" (new total for hour: ");
          Serial.print(hourly_counts[current_hour]);
          Serial.println(")");
        }
        
        Serial.println("=== NIGHT COUNT CORRECTION APPLIED ===");
        Serial.print("Correction value: ");
        Serial.println(correction_value);
        Serial.print("Old night_count: ");
        Serial.print(old_night_count);
        Serial.print(" -> New night_count: ");
        Serial.println(night_count);
        Serial.print("Old total_count: ");
        Serial.print(old_total_count);
        Serial.print(" -> New total_count: ");
        Serial.println(total_count);
        Serial.println("====================================");
        
        // Save corrected values to NVS
        saveDataToNVS();
        
        // Send corrected values to ThingsBoard with complete data
        String correction_payload = "{\"day_count\":" + String(day_count) + 
                                   ",\"night_count\":" + String(night_count) +
                                   ",\"total_count\":" + String(total_count);
        
        // Add current hour production count only
        if (current_hour != -1) {
          correction_payload += ",\"total_for_hour\":" + String(hourly_counts[current_hour]);
        }
        
        // Add the applied night correction value back to dashboard
        correction_payload += ",\"night_correction_dash\":" + String(correction_value);
        
        // Add all user inputs if available
        if (day_assignee_received && received_day_assignee.length() > 0) {
          correction_payload += ",\"day_assignee_dash\":\"" + received_day_assignee + "\"";
        }
        if (specific_item_received && received_specific_item.length() > 0) {
          correction_payload += ",\"Specific_Item_dash\":\"" + received_specific_item + "\"";
        }
        if (colour_code_received && received_colour_code.length() > 0) {
          correction_payload += ",\"Colour_code_dash\":\"" + received_colour_code + "\"";
        }
        if (target_received) {
          correction_payload += ",\"target_dash\":" + String(received_target);
        }
        if (night_assignee_received && received_night_assignee.length() > 0) {
          correction_payload += ",\"night_assignee_dash\":\"" + received_night_assignee + "\"";
        }
        
        // Add timestamp
        String current_time = getCurrentTime();
        if (current_time.length() > 0) {
          correction_payload += ",\"timestamp\":\"" + current_time + "\"";
        }
        
        correction_payload += "}";
        
        client.publish("v1/devices/me/telemetry", correction_payload.c_str());
        Serial.println("✓ Night correction data sent to ThingsBoard");
        Serial.print("   JSON sent: ");
        Serial.println(correction_payload);
      } else {
        Serial.println("⚠ Night correction value is 0 - no correction applied");
      }
    } else {
      Serial.println("✗ Error parsing night_correction - colon not found");
    }
  }
  
  // Also check for any shared attribute response that contains moulds_Day
  if (String(topic).indexOf("attributes/response") != -1 || String(topic).indexOf("attributes") != -1) {
    Serial.println("Processing shared attributes response...");
    // Parse JSON-like structure more carefully
    if (incoming.indexOf("moulds_Day") != -1) {
      Serial.println("Found moulds_Day in attributes response");
    }
  }
}

void requestSharedAttributes() {
  Serial.println("Requesting shared attributes from ThingsBoard...");
  
  // Method 1: Request all shared attributes
  String request_payload = "{}";
  bool result1 = client.publish("v1/devices/me/attributes/request/1", request_payload.c_str());
  Serial.print("Shared attributes request sent: ");
  Serial.println(result1 ? "SUCCESS" : "FAILED");
  
  delay(500); // Wait a bit between requests
  
  // Method 2: Request specific shared attributes for both shifts
  String specific_request = "{\"sharedKeys\":\"moulds_Day,moulds_Night,day_correction,night_correction,Op_time,day_assignee,Specific_Item,Colour_code,target,night_assignee,damage\"}";
  bool result2 = client.publish("v1/devices/me/attributes/request/2", specific_request.c_str());
  Serial.print("Specific moulds (Day & Night) + corrections + Op_time + all user inputs + damage request sent: ");
  Serial.println(result2 ? "SUCCESS" : "FAILED");
  
  // Method 3: Subscribe to attribute updates (in case value changes)
  bool result3 = client.subscribe("v1/devices/me/attributes");
  Serial.print("Re-subscribed to attributes: ");
  Serial.println(result3 ? "SUCCESS" : "FAILED");
}

void saveDataToNVS() {
  preferences.begin("production", false);
  preferences.putInt("day_count", day_count);
  preferences.putInt("night_count", night_count);
  preferences.putInt("total_count", total_count);
  preferences.putInt("moulds_day", received_moulds_day);
  preferences.putInt("moulds_night", received_moulds_night);
  preferences.putInt("op_time", received_op_time);
  preferences.putString("day_assignee", received_day_assignee);
  preferences.putString("specific_item", received_specific_item);
  preferences.putString("colour_code", received_colour_code);
  preferences.putInt("target", received_target);
  preferences.putString("night_assignee", received_night_assignee);
  preferences.putInt("damage", received_damage);
  preferences.putBool("day_received", moulds_day_received);
  preferences.putBool("night_received", moulds_night_received);
  preferences.putBool("op_time_recv", op_time_received);
  preferences.putBool("assignee_recv", day_assignee_received);
  preferences.putBool("item_recv", specific_item_received);
  preferences.putBool("colour_recv", colour_code_received);
  preferences.putBool("target_recv", target_received);
  preferences.putBool("night_assign_recv", night_assignee_received);
  preferences.putBool("damage_recv", damage_received);
  preferences.putInt("last_day_corr", last_day_correction);      // Save last day correction
  preferences.putInt("last_night_corr", last_night_correction);  // Save last night correction
  preferences.putBool("day_corr_app", day_correction_applied);   // Save day correction applied flag
  preferences.putBool("night_corr_app", night_correction_applied); // Save night correction applied flag
  preferences.putString("last_reset", last_reset_date);
  preferences.putULong("last_press_time", last_button_time);  // Save last button press time
  preferences.putULong("prev_timestamp", previous_button_timestamp);  // Save previous button timestamp
  preferences.putInt("cur_hr_cnt", current_hour_count);      // Shortened key name
  preferences.putInt("last_hr", last_tracked_hour);          // Shortened key name
  
  // Save hourly counts array with shorter keys
  for (int i = 0; i < 24; i++) {
    String key = "h" + String(i);  // Shortened from "hour_" to "h"
    preferences.putInt(key.c_str(), hourly_counts[i]);
  }
  
  preferences.end();
  Serial.println("Data saved to NVS");
}

void loadDataFromNVS() {
  preferences.begin("production", true);
  day_count = preferences.getInt("day_count", 0);
  night_count = preferences.getInt("night_count", 0);
  total_count = preferences.getInt("total_count", 0);
  received_moulds_day = preferences.getInt("moulds_day", 0);
  received_moulds_night = preferences.getInt("moulds_night", 0);
  received_op_time = preferences.getInt("op_time", 0);  // NO DEFAULT - must be set by user
  received_day_assignee = preferences.getString("day_assignee", "");
  received_specific_item = preferences.getString("specific_item", "");
  received_colour_code = preferences.getString("colour_code", "");
  received_target = preferences.getInt("target", 0);
  received_night_assignee = preferences.getString("night_assignee", "");
  received_damage = preferences.getInt("damage", 0);
  moulds_day_received = preferences.getBool("day_received", false);
  moulds_night_received = preferences.getBool("night_received", false);
  op_time_received = preferences.getBool("op_time_recv", false);
  day_assignee_received = preferences.getBool("assignee_recv", false);
  specific_item_received = preferences.getBool("item_recv", false);
  colour_code_received = preferences.getBool("colour_recv", false);
  target_received = preferences.getBool("target_recv", false);
  night_assignee_received = preferences.getBool("night_assign_recv", false);
  damage_received = preferences.getBool("damage_recv", false);
  last_day_correction = preferences.getInt("last_day_corr", 0);      // Load last day correction
  last_night_correction = preferences.getInt("last_night_corr", 0);  // Load last night correction
  day_correction_applied = preferences.getBool("day_corr_app", false);   // Load day correction applied flag
  night_correction_applied = preferences.getBool("night_corr_app", false); // Load night correction applied flag
  last_reset_date = preferences.getString("last_reset", "");
  last_button_time = preferences.getULong("last_press_time", 0);  // Load last button press time
  previous_button_timestamp = preferences.getULong("prev_timestamp", 0);  // Load previous button timestamp
  current_hour_count = preferences.getInt("cur_hr_cnt", 0);      // Shortened key name
  last_tracked_hour = preferences.getInt("last_hr", -1);         // Shortened key name
  
  // Load hourly counts array with shorter keys
  for (int i = 0; i < 24; i++) {
    String key = "h" + String(i);  // Shortened from "hour_" to "h"
    hourly_counts[i] = preferences.getInt(key.c_str(), 0);
  }
  
  preferences.end();
  
  Serial.print("Data loaded from NVS:");
  Serial.print(" day_count: ");
  Serial.print(day_count);
  Serial.print(", night_count: ");
  Serial.print(night_count);
  Serial.print(", total_count: ");
  Serial.print(total_count);
  Serial.print(", day_moulds: ");
  Serial.print(received_moulds_day);
  Serial.print(" (received: ");
  Serial.print(moulds_day_received ? "true" : "false");
  Serial.print("), night_moulds: ");
  Serial.print(received_moulds_night);
  Serial.print(" (received: ");
  Serial.print(moulds_night_received ? "true" : "false");
  Serial.print("), op_time: ");
  Serial.print(received_op_time);
  Serial.print("s (received: ");
  Serial.print(op_time_received ? "true" : "false");
  Serial.print("), day_assignee: '");
  Serial.print(received_day_assignee);
  Serial.print("' (received: ");
  Serial.print(day_assignee_received ? "true" : "false");
  Serial.print("), specific_item: '");
  Serial.print(received_specific_item);
  Serial.print("' (received: ");
  Serial.print(specific_item_received ? "true" : "false");
  Serial.print("), colour_code: '");
  Serial.print(received_colour_code);
  Serial.print("' (received: ");
  Serial.print(colour_code_received ? "true" : "false");
  Serial.print("), target: ");
  Serial.print(received_target);
  Serial.print(" (received: ");
  Serial.print(target_received ? "true" : "false");
  Serial.print("), night_assignee: '");
  Serial.print(received_night_assignee);
  Serial.print("' (received: ");
  Serial.print(night_assignee_received ? "true" : "false");
  Serial.print("), last_reset: ");
  Serial.print(last_reset_date);
  Serial.print(", last_press_time: ");
  Serial.print(last_button_time);
  Serial.print(", prev_timestamp: ");
  Serial.print(previous_button_timestamp);
  Serial.print(", current_hour_count: ");
  Serial.print(current_hour_count);
  Serial.print(", last_tracked_hour: ");
  Serial.println(last_tracked_hour);
}

String getCurrentDate() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "";
  }
  char dateString[20];
  strftime(dateString, sizeof(dateString), "%Y-%m-%d", &timeinfo);
  return String(dateString);
}

String getCurrentTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "";
  }
  char timeString[10];
  strftime(timeString, sizeof(timeString), "%H:%M:%S", &timeinfo);
  return String(timeString);
}

bool isTime6AM() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return false;
  }
  return (timeinfo.tm_hour == 6 && timeinfo.tm_min == 0 && timeinfo.tm_sec < 10);
}

bool isDayShift() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return true; // Default to day shift if time not available
  }
  // Day shift: 6 AM (6) to 6 PM (18) 
  return (timeinfo.tm_hour >= 6 && timeinfo.tm_hour < 18);
}

int getCurrentMoulds() {
  if (isDayShift()) {
    return received_moulds_day;
  } else {
    return received_moulds_night;
  }
}

bool isCurrentShiftConfigured() {
  if (isDayShift()) {
    return moulds_day_received;
  } else {
    return moulds_night_received;
  }
}

String getCurrentShiftName() {
  return isDayShift() ? "Day" : "Night";
}

int getCurrentHour() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return -1; // Return -1 if time not available
  }
  return timeinfo.tm_hour; // Returns 0-23
}

void checkHourlyUpdate() {
  int current_hour = getCurrentHour();
  
  if (current_hour == -1) {
    return; // Can't check if time not available
  }
  
  // Initialize last_tracked_hour if it's not set yet
  if (last_tracked_hour == -1) {
    last_tracked_hour = current_hour;
    Serial.print("🕐 Initializing hour tracking: Hour ");
    Serial.println(current_hour);
    saveDataToNVS();
    return; // Don't send data on first initialization
  }
  
  // Check if hour has changed
  if (current_hour != last_tracked_hour) {
    // Hour changed - send previous hour's data BEFORE updating last_tracked_hour
    int previous_hour = last_tracked_hour;
    Serial.print("🕐 Hour change detected: ");
    Serial.print(previous_hour);
    Serial.print(" -> ");
    Serial.println(current_hour);
    Serial.print("   Previous hour ");
    Serial.print(previous_hour);
    Serial.print(" had ");
    Serial.print(hourly_counts[previous_hour]);
    Serial.println(" pieces");
    
    sendHourlyData(previous_hour);
    
    // Update to new hour
    last_tracked_hour = current_hour;
    current_hour_count = 0;
    Serial.print("🕐 Now tracking hour: ");
    Serial.print(current_hour);
    Serial.println(" - Reset hourly counter");
    
    saveDataToNVS();
  }
}

void sendHourlyData(int hour_to_send) {
  if (hour_to_send == -1 || hour_to_send < 0 || hour_to_send > 23) {
    Serial.print("⚠ Invalid hour for sending data: ");
    Serial.println(hour_to_send);
    return; // Invalid hour
  }
  
  // Send the actual hourly production count for the specified hour with detailed info
  int hourly_production = hourly_counts[hour_to_send];
  String hourly_payload = "{\"hourly_count\":" + String(hourly_production) + 
                         ",\"completed_hour\":" + String(hour_to_send) +
                         ",\"total_for_hour\":" + String(hourly_production) +
                         ",\"hour_completed_at\":\"" + getCurrentTime() + "\"}";
  
  if (client.connected()) {
    bool result = client.publish("v1/devices/me/telemetry", hourly_payload.c_str());
    if (result) {
      Serial.println("📊 Hourly data sent to ThingsBoard:");
      Serial.print("   Hour ");
      Serial.print(hour_to_send);
      Serial.print(": ");
      Serial.print(hourly_production);
      Serial.println(" pieces produced");
      Serial.print("   JSON sent: ");
      Serial.println(hourly_payload);
    } else {
      Serial.println("✗ Failed to send hourly data to ThingsBoard");
    }
  } else {
    Serial.println("✗ Not connected - cannot send hourly data");
    Serial.print("   Would have sent Hour ");
    Serial.print(hour_to_send);
    Serial.print(": ");
    Serial.println(hourly_production);
  }
}

void checkDailyReset() {
  String currentDate = getCurrentDate();
  String currentTime = getCurrentTime();
  
  // Get current hour to check if it's 6 AM or later
  struct tm timeinfo;
  bool timeAvailable = getLocalTime(&timeinfo);
  int currentHour = timeAvailable ? timeinfo.tm_hour : -1;
  
  // Check if we need to reset for a new day - ONLY if it's 6 AM or later
  if (currentDate != last_reset_date && last_reset_date.length() > 0 && currentHour >= 6) {
    Serial.println("=== NEW DAY DETECTED (6 AM OR LATER) - DAILY RESET ===");
    Serial.print("Last reset date: ");
    Serial.print(last_reset_date);
    Serial.print(" | Current date: ");
    Serial.println(currentDate);
    Serial.print("Current time: ");
    Serial.print(currentTime);
    Serial.print(" | Current hour: ");
    Serial.println(currentHour);
    Serial.print("Previous day_count: ");
    Serial.print(day_count);
    Serial.print(", night_count: ");
    Serial.print(night_count);
    Serial.print(", total_count: ");
    Serial.println(total_count);
    
    // Reset ALL counters for new day
    day_count = 0;
    night_count = 0;
    total_count = 0;
    current_hour_count = 0;
    // Keep tracking current hour - don't reset to -1
    
    // Reset correction flags for new day
    day_correction_applied = false;
    night_correction_applied = false;
    last_day_correction = 0;
    last_night_correction = 0;
    
    // Reset all hourly counts
    for (int i = 0; i < 24; i++) {
      hourly_counts[i] = 0;
    }
    
    last_reset_date = currentDate;
    
    // Reset button timing to allow immediate press after reset
    last_button_time = 0;
    
    saveDataToNVS();
    
    Serial.print("Reset complete. New counts - day: ");
    Serial.print(day_count);
    Serial.print(", night: ");
    Serial.print(night_count);
    Serial.print(", total: ");
    Serial.println(total_count);
    Serial.print("Reset date recorded: ");
    Serial.println(last_reset_date);
    Serial.println("✓ Ready for new day operations!");
    
    // Send reset notification to ThingsBoard
    String reset_payload = "{\"day_count\":" + String(day_count) + 
                          ",\"night_count\":" + String(night_count) +
                          ",\"total_count\":" + String(total_count) + "}";
    client.publish("v1/devices/me/telemetry", reset_payload.c_str());
    return; // Exit after reset
  }
  
  // If it's before 6 AM on a new day, just log but don't reset
  if (currentDate != last_reset_date && last_reset_date.length() > 0 && currentHour >= 0 && currentHour < 6) {
    Serial.println("=== NEW DAY DETECTED BUT BEFORE 6 AM - NO RESET ===");
    Serial.print("Current time: ");
    Serial.print(currentTime);
    Serial.print(" | Current hour: ");
    Serial.println(currentHour);
    Serial.println("Waiting until 6 AM or later for daily reset...");
    return; // Don't reset yet
  }
  
  // Also check if it's exactly 6 AM for immediate reset (if device is on)
  if (isTime6AM() && currentDate != last_reset_date) {
    Serial.println("=== EXACT 6:00 AM RESET ===");
    Serial.print("Previous day_count: ");
    Serial.print(day_count);
    Serial.print(", night_count: ");
    Serial.print(night_count);
    Serial.print(", total_count: ");
    Serial.println(total_count);
    
    // Reset ALL counters for new day
    day_count = 0;
    night_count = 0;
    total_count = 0;
    current_hour_count = 0;
    // Keep tracking current hour - don't reset to -1
    
    // Reset all hourly counts
    for (int i = 0; i < 24; i++) {
      hourly_counts[i] = 0;
    }
    
    last_reset_date = currentDate;
    
    // Reset button timing to allow immediate press after reset
    last_button_time = 0;
    
    saveDataToNVS();
    
    Serial.print("6 AM Reset complete. New counts - day: ");
    Serial.print(day_count);
    Serial.print(", night: ");
    Serial.print(night_count);
    Serial.print(", total: ");
    Serial.println(total_count);
    Serial.print("Reset date recorded: ");
    Serial.println(last_reset_date);
    Serial.println("✓ Ready for new day operations!");
    
    // Send reset notification to ThingsBoard
    String reset_payload = "{\"day_count\":" + String(day_count) + 
                          ",\"night_count\":" + String(night_count) +
                          ",\"total_count\":" + String(total_count) + "}";
    client.publish("v1/devices/me/telemetry", reset_payload.c_str());
  }
}

void checkMissedDailyReset() {
  String currentDate = getCurrentDate();
  
  // Get current hour to check if it's 6 AM or later
  struct tm timeinfo;
  bool timeAvailable = getLocalTime(&timeinfo);
  int currentHour = timeAvailable ? timeinfo.tm_hour : -1;
  
  // Only reset if date has actually changed AND we have existing counts AND it's 6 AM or later
  if (last_reset_date.length() > 0 && last_reset_date != currentDate && currentHour >= 6) {
    Serial.println("=== DATE CHANGE DETECTED (6 AM OR LATER) ===");
    Serial.print("🔄 Last reset: ");
    Serial.print(last_reset_date);
    Serial.print(" | Current date: ");
    Serial.println(currentDate);
    Serial.print("Current hour: ");
    Serial.println(currentHour);
    
    if (day_count > 0 || night_count > 0 || total_count > 0) {
      Serial.println("Found existing counts from previous day - forcing reset!");
      Serial.print("Previous values: day_count=");
      Serial.print(day_count);
      Serial.print(", night_count=");
      Serial.print(night_count);
      Serial.print(", total_count=");
      Serial.println(total_count);
      
      // Reset ALL counters for new day
      day_count = 0;
      night_count = 0;
      total_count = 0;
      current_hour_count = 0;
      // Keep tracking current hour - don't reset to -1
      
      // Reset all hourly counts
      for (int i = 0; i < 24; i++) {
        hourly_counts[i] = 0;
      }
      
      last_reset_date = currentDate;
      
      saveDataToNVS();
      
      Serial.println("✓ Date-based reset completed - ready for new day operations!");
      
      // Send reset notification to ThingsBoard
      String reset_payload = "{\"day_count\":" + String(day_count) + 
                            ",\"night_count\":" + String(night_count) +
                            ",\"total_count\":" + String(total_count) + "}";
      client.publish("v1/devices/me/telemetry", reset_payload.c_str());
    } else {
      Serial.println("Counts already zero - just updating reset date");
      last_reset_date = currentDate;
      saveDataToNVS();
    }
  } else if (last_reset_date.length() > 0 && last_reset_date != currentDate && currentHour >= 0 && currentHour < 6) {
    Serial.println("=== DATE CHANGE DETECTED BUT BEFORE 6 AM - NO RESET ===");
    Serial.print("Current hour: ");
    Serial.println(currentHour);
    Serial.println("Waiting until 6 AM or later for daily reset...");
  } else if (last_reset_date.length() == 0) {
    Serial.println("No reset date found - setting current date without reset");
    last_reset_date = currentDate;
    saveDataToNVS();
  } else {
    Serial.println("✓ Same day - no reset needed");
  }
}

void processButtonPress() {
  Serial.println("=== PROCESSING BUTTON PRESS ===");
  
  /*
  ==================== NOISE REJECTION AND TIMESTAMP MANAGEMENT ====================
  
  PROBLEM: Industrial environments have electrical noise that can trigger false button presses.
  If we update the timestamp for every press (including noise), it corrupts future timing validation.
  
  SOLUTION: Only update previous_button_timestamp when we have a VALID count.
  
  EXAMPLES OF DIFFERENT SCENARIOS:
  
  📍 SCENARIO 1: NORMAL VALID SEQUENCE
  Press 1 (Valid): T=1000ms  -> Count=1, previous_timestamp=1000
  Press 2 (Valid): T=31000ms -> Diff=30s, Valid (Op_time=25s+5s tolerance), Count=2, previous_timestamp=31000
  Press 3 (Valid): T=61000ms -> Diff=30s, Valid, Count=3, previous_timestamp=61000
  
  📍 SCENARIO 2: NOISE REJECTION
  Press 1 (Valid): T=1000ms  -> Count=1, previous_timestamp=1000
  Press 2 (NOISE): T=5000ms  -> Diff=4s, INVALID (too fast), Count=1, previous_timestamp=1000 (UNCHANGED)
  Press 3 (Valid): T=31000ms -> Diff=30s (from Press 1!), Valid, Count=2, previous_timestamp=31000
  
  📍 SCENARIO 3: MULTIPLE NOISE EVENTS
  Press 1 (Valid): T=1000ms  -> Count=1, previous_timestamp=1000
  Press 2 (NOISE): T=3000ms  -> INVALID, Count=1, previous_timestamp=1000 (UNCHANGED)
  Press 3 (NOISE): T=8000ms  -> INVALID, Count=1, previous_timestamp=1000 (UNCHANGED)
  Press 4 (Valid): T=31000ms -> Diff=30s (from Press 1!), Valid, Count=2, previous_timestamp=31000
  
  📍 SCENARIO 4: LONG IDLE PERIOD (Machine stopped and restarted)
  Press 1 (Valid): T=1000ms     -> Count=1, previous_timestamp=1000
  Press 2 (Valid): T=1201000ms  -> Diff=20min, LONG IDLE, Valid restart, Count=2, previous_timestamp=1201000
  
  📍 SCENARIO 5: NOISE AFTER LONG IDLE
  Press 1 (Valid): T=1000ms     -> Count=1, previous_timestamp=1000
  Press 2 (Valid): T=1201000ms  -> Diff=20min, LONG IDLE, Valid restart, Count=2, previous_timestamp=1201000
  Press 3 (NOISE): T=1205000ms  -> Diff=4s, INVALID (too fast), Count=2, previous_timestamp=1201000 (UNCHANGED)
  Press 4 (Valid): T=1231000ms  -> Diff=30s (from Press 2!), Valid, Count=3, previous_timestamp=1231000
  
  KEY BENEFITS:
  ✅ Noise doesn't corrupt timing validation for legitimate presses
  ✅ Consecutive noise events don't accumulate timing errors
  ✅ Valid presses maintain proper cycle timing validation
  ✅ Long idle periods still work correctly
  ✅ Industrial noise immunity while maintaining production accuracy
  
  =================================================================================
  */
  
  // First, check for missed daily reset before processing the button press
  checkMissedDailyReset();
  
  Serial.print("Current time: ");
  Serial.print(getCurrentTime());
  Serial.print(" | Current shift: ");
  Serial.println(getCurrentShiftName());
  
  // Get current timestamp
  unsigned long current_timestamp = millis();
  
  // Validate cycle time if we have previous timestamp and Op_time is configured
  if (previous_button_timestamp > 0 && op_time_received) {
    unsigned long time_difference = current_timestamp - previous_button_timestamp;
    unsigned long min_cycle_time = ATLEAST + (received_op_time * 1000);  // Convert seconds to milliseconds
    unsigned long max_cycle_time = ATMOST + (received_op_time * 1000);   // Convert seconds to milliseconds
    
    Serial.println("=== CYCLE TIME VALIDATION ===");
    Serial.println("🔍 NOISE REJECTION ANALYSIS:");
    Serial.print("Previous VALID timestamp: ");
    Serial.println(previous_button_timestamp);
    Serial.print("Current timestamp: ");
    Serial.println(current_timestamp);
    Serial.print("Time difference: ");
    Serial.print(time_difference);
    Serial.print(" ms (");
    Serial.print(time_difference / 1000.0, 1);
    Serial.println(" seconds)");
    Serial.print("Valid range: ");
    Serial.print(min_cycle_time);
    Serial.print(" ms to ");
    Serial.print(max_cycle_time);
    Serial.println(" ms");
    Serial.print("Exceed limit: ");
    Serial.print(EXCEED_LIMIT);
    Serial.print(" ms (");
    Serial.print(EXCEED_LIMIT / 60000.0, 1);
    Serial.println(" minutes)");
    
    // Check if machine was idle for more than 18 minutes
    if (time_difference > EXCEED_LIMIT) {
      Serial.println("📍 SCENARIO 4: LONG IDLE PERIOD DETECTED");
      Serial.println("✓ Accepting as valid production restart");
      Serial.print("   Machine was idle for ");
      Serial.print(time_difference / 60000.0, 1);
      Serial.println(" minutes - considering as production restart");
      Serial.println("   This is a valid count after extended downtime");
    }
    // Check if cycle time is outside valid range (but not a long idle period)
    else if (time_difference < min_cycle_time || time_difference > max_cycle_time) {
      Serial.println("📍 SCENARIO 2/3: NOISE DETECTED");
      Serial.println("⚠ INVALID CYCLE TIME - Button press rejected (NOISE)!");
      Serial.print("   Time difference ");
      Serial.print(time_difference);
      Serial.print(" ms is outside valid range (");
      Serial.print(min_cycle_time);
      Serial.print(" - ");
      Serial.print(max_cycle_time);
      Serial.println(" ms)");
      
      if (time_difference < min_cycle_time) {
        Serial.println("   ⚡ TOO FAST - Likely electrical noise or vibration");
      } else {
        Serial.println("   🐌 TOO SLOW - Cycle time exceeded but not long enough for idle period");
      }
      
      Serial.println("   🚫 This button press will NOT be counted");
      Serial.println("   🕒 CRITICAL: Timestamp NOT updated - next valid press will calculate from LAST VALID press");
      Serial.print("   📌 Next press will calculate timing from timestamp: ");
      Serial.println(previous_button_timestamp);
      Serial.println("===============================");
      
      // DO NOT update timestamp for invalid counts (noise rejection)
      // Keep previous_button_timestamp unchanged so next valid press calculates from last valid press
      return; // Exit without counting and without updating timestamp
    } else {
      Serial.println("📍 SCENARIO 1: NORMAL VALID SEQUENCE");
      Serial.println("✓ Valid cycle time - proceeding with count");
      Serial.println("   This press fits within expected production timing");
    }
    Serial.println("============================");
  } else if (!op_time_received || received_op_time <= 0) {
    Serial.println("❌ Op_time not configured or invalid - BUTTON PRESS REJECTED!");
    Serial.println("   Please set Op_time value from dashboard first");
    Serial.println("   Current Op_time: " + String(received_op_time) + " seconds");
    Serial.println("===============================");
    // Don't update timestamp, don't count, just return
    return; // Exit without counting
  } else {
    Serial.println("📍 SCENARIO: FIRST PRESS OR NO Op_time");
    Serial.println("ℹ First button press - no previous timestamp for validation");
    Serial.println("   This press will establish the baseline for future timing validation");
  }
  
  // IMPORTANT: Only update timestamp AFTER validation passes
  // This ensures only valid counts are used for timing calculations
  bool valid_count_processed = false;
  
  if (isCurrentShiftConfigured()) {
    // Calculate: button press (1) * moulds = amount to add
    int amount_to_add = 1 * getCurrentMoulds();
    
    Serial.print("Before: day_count=");
    Serial.print(day_count);
    Serial.print(", night_count=");
    Serial.print(night_count);
    Serial.print(", total_count=");
    Serial.println(total_count);
    
    if (isDayShift()) {
      // Day shift: add to day_count
      day_count = day_count + amount_to_add;
      Serial.print("DAY SHIFT - Calculation: 1 press * ");
      Serial.print(getCurrentMoulds());
      Serial.print(" moulds = ");
      Serial.print(amount_to_add);
      Serial.print(" added to day_count");
      Serial.print(" | New day_count = ");
      Serial.println(day_count);
    } else {
      // Night shift: add to night_count
      night_count = night_count + amount_to_add;
      Serial.print("NIGHT SHIFT - Calculation: 1 press * ");
      Serial.print(getCurrentMoulds());
      Serial.print(" moulds = ");
      Serial.print(amount_to_add);
      Serial.print(" added to night_count");
      Serial.print(" | New night_count = ");
      Serial.println(night_count);
    }
    
    // Update total_count = day_count + night_count
    total_count = day_count + night_count;
    
    // Update hourly tracking
    int current_hour = getCurrentHour();
    if (current_hour != -1) {
      hourly_counts[current_hour] += amount_to_add;
      current_hour_count += amount_to_add;
      Serial.print("📊 Hourly update - Hour ");
      Serial.print(current_hour);
      Serial.print(": +");
      Serial.print(amount_to_add);
      Serial.print(" (total for hour ");
      Serial.print(current_hour);
      Serial.print(": ");
      Serial.print(hourly_counts[current_hour]);
      Serial.println(")");
    }
    
    Serial.print("After: day_count=");
    Serial.print(day_count);
    Serial.print(", night_count=");
    Serial.print(night_count);
    Serial.print(", total_count=");
    Serial.println(total_count);
    
    // Mark that we successfully processed a valid count
    valid_count_processed = true;
    Serial.println("✓ VALID COUNT PROCESSED - Updating timestamp for next calculation");
    Serial.println("===========================");

    // Save to NVS immediately after update
    saveDataToNVS();

    // Send to ThingsBoard with ALL data using centralized function
    String payload = buildCompleteTelemetryPayload();
    
    if (client.connected()) {
      bool result = client.publish("v1/devices/me/telemetry", payload.c_str());
      if (result) {
        Serial.println("✓ Complete data sent to ThingsBoard successfully");
        Serial.print("   JSON sent: ");
        Serial.println(payload);
        Serial.println("   ★ This includes ALL variables for complete time-series tracking!");
      } else {
        Serial.println("✗ Failed to send data to ThingsBoard");
      }
    } else {
      Serial.println("✗ Not connected to ThingsBoard");
      Serial.print("   Would have sent: ");
      Serial.println(payload);
    }
    
  } else {
    Serial.println("No " + getCurrentShiftName() + " shift moulds value received from dashboard yet!");
    Serial.println("Please send 'moulds_" + getCurrentShiftName() + "' value from ThingsBoard dashboard first");
    Serial.println("   ⚠ Count not processed - timestamp will NOT be updated");
  }
  
  // ⭐ CRITICAL: Only update previous_button_timestamp if we successfully processed a valid count
  if (valid_count_processed) {
    previous_button_timestamp = current_timestamp;
    Serial.println("");
    Serial.println("🕒 ===== TIMESTAMP UPDATE CONFIRMATION =====");
    Serial.print("✅ Valid count processed - Timestamp updated: ");
    Serial.println(previous_button_timestamp);
    Serial.println("   Next button press will calculate timing from THIS timestamp");
    Serial.println("   Industrial noise immunity: ACTIVE");
    Serial.println("============================================");
    // Save the updated timestamp
    saveDataToNVS();
  } else {
    Serial.println("");
    Serial.println("🚫 ===== TIMESTAMP PRESERVATION =====");
    Serial.println("❌ Invalid count (noise) - Timestamp NOT updated");
    Serial.print("   Previous valid timestamp preserved: ");
    Serial.println(previous_button_timestamp);
    Serial.println("   Next valid press will calculate from LAST VALID press");
    Serial.println("   Noise rejected successfully!");
    Serial.println("====================================");
  }
}



void reconnect() {
  while (!client.connected()) {
    Serial.print("Connecting to ThingsBoard...");
    Serial.print(" (Server: ");
    Serial.print(THINGSBOARD_SERVER);
    Serial.print(":");
    Serial.print(THINGSBOARD_PORT);
    Serial.println(")");
    
    // Check WiFi connection first
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("✗ WiFi disconnected! Reconnecting...");
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
      }
      Serial.println("\n✓ WiFi reconnected!");
    }
    
    // Try connecting with a longer timeout
    client.setSocketTimeout(30);  // 30 second timeout
    
    if (client.connect("ESP32Client", TOKEN, NULL)) {
      Serial.println("✓ Connected to ThingsBoard successfully!");

      // Subscribe to shared attribute updates
      client.subscribe("v1/devices/me/attributes");
      Serial.println("✓ Subscribed to attributes");
      
      // Subscribe to shared attribute responses  
      client.subscribe("v1/devices/me/attributes/response/+");
      Serial.println("✓ Subscribed to attribute responses");
      
      // Subscribe to RPC commands for receiving moulds
      client.subscribe("v1/devices/me/rpc/request/+");
      Serial.println("✓ Subscribed to RPC commands");
      
      // Send connection confirmation with complete data
      int current_hour = getCurrentHour();
      String confirm_payload = "{\"day_count\":" + String(day_count) +
                               ",\"night_count\":" + String(night_count) +
                               ",\"total_count\":" + String(total_count);
      
      // Add current hour info if available
      if (current_hour != -1) {
        confirm_payload += ",\"current_hour\":" + String(current_hour) +
                          ",\"total_for_hour\":" + String(hourly_counts[current_hour]);
      }
      
      // Add day assignee if available
      if (day_assignee_received && received_day_assignee.length() > 0) {
        confirm_payload += ",\"day_assignee_dash\":\"" + received_day_assignee + "\"";
      }
      
      // Add timestamp
      String current_time = getCurrentTime();
      if (current_time.length() > 0) {
        confirm_payload += ",\"timestamp\":\"" + current_time + "\"";
      }
      
      confirm_payload += "}";
      
      client.publish("v1/devices/me/telemetry", confirm_payload.c_str());
      Serial.println("✓ Connection confirmed to ThingsBoard");
      Serial.print("   JSON sent: ");
      Serial.println(confirm_payload);
      
      // Request shared attributes for both shifts if not already available
      bool need_day_config = !moulds_day_received;
      bool need_night_config = !moulds_night_received;
      bool need_op_time_config = !op_time_received;
      
      if (need_day_config || need_night_config || need_op_time_config) {
        Serial.println("Requesting configurations from ThingsBoard...");
        if (need_day_config) Serial.println("- Need moulds_Day");
        if (need_night_config) Serial.println("- Need moulds_Night");
        if (need_op_time_config) Serial.println("- Need Op_time");
        
        delay(1000); // Wait for subscriptions to be established
        requestSharedAttributes();
        
        // Give it a moment to receive the response
        delay(2000);
        client.loop(); // Process any incoming messages
        
        // Check results
        if (moulds_day_received && moulds_night_received && op_time_received) {
          Serial.println("✓ Successfully retrieved all configurations!");
        } else {
          Serial.println("⚠ Partially configured:");
          if (moulds_day_received) Serial.println("  ✓ Day shift configured");
          if (moulds_night_received) Serial.println("  ✓ Night shift configured");
          if (op_time_received) {
            Serial.print("  ✓ Op_time configured: ");
            Serial.print(received_op_time);
            Serial.println(" seconds");
          }
        }
      } else {
        Serial.println("Using saved configurations:");
        Serial.print("- Day shift moulds: ");
        Serial.println(received_moulds_day);
        Serial.print("- Night shift moulds: ");
        Serial.println(received_moulds_night);
        Serial.print("- Op_time: ");
        Serial.print(received_op_time);
        Serial.println(" seconds");
      }
      
    } else {
      int error_code = client.state();
      Serial.print("✗ Connection failed, rc=");
      Serial.print(error_code);
      Serial.print(" (");
      
      // Explain the error code
      switch(error_code) {
        case -4:
          Serial.print("MQTT_CONNECTION_TIMEOUT - Server not responding");
          break;
        case -3:
          Serial.print("MQTT_CONNECTION_LOST - Network issue");
          break;
        case -2:
          Serial.print("MQTT_CONNECT_FAILED - Network failed");
          break;
        case -1:
          Serial.print("MQTT_DISCONNECTED - Client disconnected");
          break;
        case 1:
          Serial.print("MQTT_CONNECT_BAD_PROTOCOL - Bad protocol version");
          break;
        case 2:
          Serial.print("MQTT_CONNECT_BAD_CLIENT_ID - Client ID rejected");
          break;
        case 3:
          Serial.print("MQTT_CONNECT_UNAVAILABLE - Server unavailable");
          break;
        case 4:
          Serial.print("MQTT_CONNECT_BAD_CREDENTIALS - Bad username/password");
          break;
        case 5:
          Serial.print("MQTT_CONNECT_UNAUTHORIZED - Client not authorized");
          break;
        default:
          Serial.print("Unknown error");
          break;
      }
      
      Serial.println(")");
      Serial.print("WiFi Status: ");
      Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
      Serial.print("Server: ");
      Serial.print(THINGSBOARD_SERVER);
      Serial.print(":");
      Serial.println(THINGSBOARD_PORT);
      Serial.print("Token: ");
      Serial.println(TOKEN);
      Serial.println("Retrying in 10 seconds...");
      delay(10000);  // Wait 10 seconds before retry
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // Load saved data from NVS first
  loadDataFromNVS();
  
  // Show current status
  Serial.println("=== Configuration Status ===");
  Serial.print("Current date: ");
  Serial.print(getCurrentDate());
  Serial.print(" | Current time: ");
  Serial.print(getCurrentTime());
  Serial.print(" | Current shift: ");
  Serial.println(getCurrentShiftName());
  
  Serial.print("Day count: ");
  Serial.print(day_count);
  Serial.print(", Night count: ");
  Serial.print(night_count);
  Serial.print(", Total count: ");
  Serial.println(total_count);
  
  Serial.print("Last reset date: ");
  Serial.println(last_reset_date.length() > 0 ? last_reset_date : "Never");
  
  if (moulds_day_received) {
    Serial.print("✓ Day shift configured: ");
    Serial.println(received_moulds_day);
  } else {
    Serial.println("⚠ Day shift not configured");
  }
  
  if (moulds_night_received) {
    Serial.print("✓ Night shift configured: ");
    Serial.println(received_moulds_night);
  } else {
    Serial.println("⚠ Night shift not configured");
  }
  
  if (op_time_received && received_op_time > 0) {
    Serial.print("✓ Op_time configured: ");
    Serial.print(received_op_time);
    Serial.println(" seconds");
    unsigned long min_cycle = ATLEAST + (received_op_time * 1000);
    unsigned long max_cycle = ATMOST + (received_op_time * 1000);
    Serial.print("   Valid cycle range: ");
    Serial.print(min_cycle);
    Serial.print(" - ");
    Serial.print(max_cycle);
    Serial.println(" ms");
  } else {
    Serial.println("❌ Op_time NOT CONFIGURED - BUTTON PRESSES WILL BE REJECTED!");
    Serial.println("   Please set Op_time value from ThingsBoard dashboard first");
    Serial.print("   Current Op_time: ");
    Serial.print(received_op_time);
    Serial.println(" seconds");
  }
  
  if (!moulds_day_received && !moulds_night_received) {
    Serial.println("Will request configurations from ThingsBoard on connection...");
  } else if (!op_time_received || received_op_time <= 0) {
    Serial.println("⚠ WARNING: Device NOT ready - Op_time must be configured first!");
    Serial.println("   Current shift moulds configured, but Op_time missing");
    Serial.println("   Button presses will be REJECTED until Op_time is set");
  } else {
    Serial.println("✓ Device ready for " + getCurrentShiftName() + " shift button press!");
  }
  
  // Initialize button pin
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println("Button initialized on pin " + String(BUTTON_PIN));
  
  // Debug: Check initial button state
  delay(100); // Give pin time to stabilize
  bool initial_state = digitalRead(BUTTON_PIN);
  Serial.print("Initial button state: ");
  Serial.println(initial_state ? "HIGH" : "LOW");
  
  // ==== Optional: Set Static IP (Uncomment to use) ====
  // IPAddress staticIP(192, 168, 1, 100);    // Your desired static IP
  // IPAddress gateway(192, 168, 1, 1);       // Your router's IP
  // IPAddress subnet(255, 255, 255, 0);      // Subnet mask
  // IPAddress primaryDNS(8, 8, 8, 8);        // Google DNS
  // WiFi.config(staticIP, gateway, subnet, primaryDNS);
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✓ WiFi connected!");
  Serial.print("   IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("   Signal strength: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  
  // ==== Initialize OTA ====
  Serial.println("\n=== Initializing OTA ===");
  
  // Set OTA hostname (you can change this to identify your device)
  ArduinoOTA.setHostname("ESP32-Production-Monitor");
  
  // Set OTA password for security (optional but recommended)
  ArduinoOTA.setPassword(OTA_PASSWORD);
  
  // Set OTA port (default is 3232, you can change if needed)
  ArduinoOTA.setPort(3232);
  
  // OTA event handlers
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
    Serial.println("Start updating " + type);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  
  ArduinoOTA.begin();
  Serial.println("✓ OTA Ready");
  Serial.print("   Hostname: ");
  Serial.println("ESP32-Production-Monitor");
  Serial.print("   IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("   Port: ");
  Serial.println(3232);
  
  // Initialize mDNS for better network discovery
  if (!MDNS.begin("esp32-production")) {
    Serial.println("Error setting up MDNS responder!");
  } else {
    Serial.println("✓ mDNS responder started");
    MDNS.addService("http", "tcp", 80);
  }
  
  // ==== Initialize Web Server for Remote Monitoring ====
  server.on("/", [](){
    String html = "<html><head><meta http-equiv='refresh' content='5'><title>ESP32 Production Monitor</title></head>";
    html += "<body><h1>ESP32 Production Monitor</h1>";
    html += "<h2>Current Status</h2>";
    html += "<p><strong>Date:</strong> " + getCurrentDate() + "</p>";
    html += "<p><strong>Time:</strong> " + getCurrentTime() + "</p>";
    html += "<p><strong>Shift:</strong> " + getCurrentShiftName() + "</p>";
    html += "<p><strong>Day Count:</strong> " + String(day_count) + "</p>";
    html += "<p><strong>Night Count:</strong> " + String(night_count) + "</p>";
    html += "<p><strong>Total Count:</strong> " + String(total_count) + "</p>";
    html += "<p><strong>WiFi Signal:</strong> " + String(WiFi.RSSI()) + " dBm</p>";
    html += "<p><strong>IP Address:</strong> " + WiFi.localIP().toString() + "</p>";
    html += "<p><em>Auto-refresh every 5 seconds</em></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });
  
  server.begin();
  Serial.println("✓ Web Server Started");
  Serial.print("   Access at: http://");
  Serial.println(WiFi.localIP());
  
  // NOW initialize time after WiFi is connected
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Starting NTP time synchronization...");
  
  // Wait for time to be synchronized with a more robust check
  Serial.print("Waiting for time sync");
  int timeout = 0;
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo) && timeout < 30) {
    delay(1000);  // Wait 1 second between checks
    Serial.print(".");
    timeout++;
  }
  Serial.println();
  
  if (!getLocalTime(&timeinfo)) {
    Serial.println("⚠ Warning: Could not sync with NTP server - skipping reset checks");
  } else {
    Serial.print("✓ Current date obtained: ");
    Serial.println(getCurrentDate());
    Serial.print("✓ Current time: ");
    Serial.println(getCurrentTime());
    
    // Check if we need to force reset for today (only if date actually changed AND it's 6 AM or later)
    String today = getCurrentDate();
    int currentHour = timeinfo.tm_hour;
    
    if (today.length() > 0 && last_reset_date.length() > 0 && last_reset_date != today && currentHour >= 6) {
      Serial.println("⚠ WARNING: Counts are from previous day - forcing reset for new day (6 AM or later)!");
      Serial.print("Last reset: ");
      Serial.print(last_reset_date);
      Serial.print(" | Today: ");
      Serial.println(today);
      Serial.print("Current hour: ");
      Serial.println(currentHour);
      Serial.println("Forcing reset now...");
      
      Serial.print("Old values - day_count: ");
      Serial.print(day_count);
      Serial.print(", night_count: ");
      Serial.print(night_count);
      Serial.print(", total_count: ");
      Serial.println(total_count);
      
      day_count = 0;
      night_count = 0;
      total_count = 0;
      current_hour_count = 0;
      // Keep tracking current hour - don't reset to -1
      
      // Reset all hourly counts
      for (int i = 0; i < 24; i++) {
        hourly_counts[i] = 0;
      }
      
      last_reset_date = today;
      last_button_time = 0;  // Allow immediate button press
      saveDataToNVS();
      
      Serial.print("New values - day_count: ");
      Serial.print(day_count);
      Serial.print(", night_count: ");
      Serial.print(night_count);
      Serial.print(", total_count: ");
      Serial.println(total_count);
      Serial.println("✓ Forced reset completed for today");
    } else if (today.length() > 0 && last_reset_date.length() > 0 && last_reset_date != today && currentHour < 6) {
      Serial.println("⚠ WARNING: Counts are from previous day but it's before 6 AM - NO RESET");
      Serial.print("Last reset: ");
      Serial.print(last_reset_date);
      Serial.print(" | Today: ");
      Serial.println(today);
      Serial.print("Current hour: ");
      Serial.println(currentHour);
      Serial.println("Will reset at 6 AM or later...");
    } else if (today.length() > 0 && last_reset_date.length() == 0) {
      // First time setup - set today as reset date WITHOUT resetting counts
      Serial.println("📅 First time setup - setting today as reset date");
      last_reset_date = today;
      saveDataToNVS();
      Serial.print("Reset date set to: ");
      Serial.println(last_reset_date);
      Serial.println("✓ Keeping existing counts - no reset needed");
    } else if (today.length() > 0) {
      Serial.println("✓ Counts are from today - no reset needed");
    }
  }
  
  client.setServer(THINGSBOARD_SERVER, THINGSBOARD_PORT);
  client.setCallback(callback);
  client.setKeepAlive(60);  // 60 second keepalive
  client.setSocketTimeout(30);  // 30 second socket timeout
  
  Serial.println("\n=== MQTT Configuration ===");
  Serial.print("ThingsBoard Server: ");
  Serial.println(THINGSBOARD_SERVER);
  Serial.print("Port: ");
  Serial.println(THINGSBOARD_PORT);
  Serial.print("Device Token: ");
  Serial.println(TOKEN);
  Serial.println("==========================");
}

void loop() {
  // Check WiFi connection first
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ WiFi connection lost! Attempting to reconnect...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\n✓ WiFi reconnected!");
  }
  
  if (!client.connected()) {
    Serial.println("⚠ MQTT connection lost! Attempting to reconnect...");
    reconnect();
  }
  client.loop();
  
  // Handle OTA updates
  ArduinoOTA.handle();
  
  // Handle web server requests
  server.handleClient();
  
  // Check for daily reset at 6 AM
  checkDailyReset();
  
  // Check for hourly updates
  checkHourlyUpdate();
  
  // Simple button detection
  bool button_reading = digitalRead(BUTTON_PIN);
  
  // Check for button press (HIGH to LOW transition)
  if (button_reading == LOW && last_button_state == HIGH) {
    // Check if enough time has passed to avoid multiple triggers (debounce)
    if (millis() - last_button_time > button_debounce) {
      
      // Check if enough time has passed since last VALID button press (accidental prevention)
      if (millis() - last_button_time > ACCIDENTLY_PRESSED) {
        Serial.println("✓ Valid button press detected (>5 seconds since last press)");
        processButtonPress();
        last_button_time = millis();  // Update last valid press time
      } else {
        unsigned long time_remaining = ACCIDENTLY_PRESSED - (millis() - last_button_time);
        Serial.print("⚠ Button press ignored - too soon! Wait ");
        Serial.print(time_remaining / 1000.0, 1);
        Serial.println(" more seconds for next valid press");
      }
    }
  }
  
  last_button_state = button_reading;
  
  // Small delay to prevent excessive polling
  delay(10);
}
