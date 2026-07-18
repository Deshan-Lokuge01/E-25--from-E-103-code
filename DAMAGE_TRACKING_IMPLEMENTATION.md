# Damage Tracking Implementation

## 🎯 Overview
This implementation adds comprehensive damage tracking functionality that allows users to input damage counts from the dashboard, automatically subtracts them from the appropriate shift counts, and maintains a timeseries record of all damage events.

## 🔧 Key Features Implemented

### 1. **Global Variables Added**
```cpp
int received_damage = 0;           // Damage count input from dashboard
bool damage_received = false;      // Track if damage has been received from dashboard
```

### 2. **MQTT Callback Processing**
- Processes "damage" key from dashboard input
- Validates damage value (must be > 0)
- Determines current shift (Day/Night) for appropriate count subtraction
- Updates production counts automatically
- Sends damage_dash to dashboard for timeseries tracking

### 3. **NVS Storage Integration**
- Saves damage variables to Non-Volatile Storage
- Preserves damage tracking state across power cycles
- Loads damage configuration on startup

### 4. **Automatic Count Management**
- Subtracts damage from appropriate shift based on input time
- Updates total_count automatically
- Prevents negative counts (uses max(0, count - damage))
- Maintains data integrity

### 5. **Dashboard Integration**
- Requests "damage" from shared attributes
- Sends "damage_dash" for timeseries widget
- Includes all production data in telemetry

## 📊 Detailed Scenarios with Examples

### 📍 SCENARIO 1: Day Shift Damage Input
```
Time: 10:30 AM (Day Shift)
Current Counts: day_count=100, night_count=50, total_count=150
User Input: damage=5

Processing:
✅ Current shift identified: Day
✅ Subtract from day_count: 100 - 5 = 95
✅ Recalculate total: 95 + 50 = 145
✅ Send to dashboard: damage_dash=5, day_count=95, night_count=50, total_count=145

Result: day_count=95, night_count=50, total_count=145
```

### 📍 SCENARIO 2: Night Shift Damage Input
```
Time: 11:45 PM (Night Shift)
Current Counts: day_count=120, night_count=80, total_count=200
User Input: damage=3

Processing:
✅ Current shift identified: Night
✅ Subtract from night_count: 80 - 3 = 77
✅ Recalculate total: 120 + 77 = 197
✅ Send to dashboard: damage_dash=3, day_count=120, night_count=77, total_count=197

Result: day_count=120, night_count=77, total_count=197
```

### 📍 SCENARIO 3: Large Damage Exceeding Count
```
Time: 2:15 PM (Day Shift)
Current Counts: day_count=8, night_count=25, total_count=33
User Input: damage=15

Processing:
✅ Current shift identified: Day
✅ Prevent negative: max(0, 8 - 15) = 0
✅ Recalculate total: 0 + 25 = 25
✅ Send to dashboard: damage_dash=15, day_count=0, night_count=25, total_count=25

Result: day_count=0, night_count=25, total_count=25
```

### 📍 SCENARIO 4: Multiple Damage Events
```
Event 1 - Time: 9:00 AM (Day Shift)
Current Counts: day_count=50, night_count=30, total_count=80
User Input: damage=2
Result: day_count=48, night_count=30, total_count=78

Event 2 - Time: 10:30 PM (Night Shift)  
Current Counts: day_count=48, night_count=35, total_count=83
User Input: damage=1
Result: day_count=48, night_count=34, total_count=82

Each event tracked separately in timeseries with damage_dash
```

### 📍 SCENARIO 5: Invalid Damage Input
```
User Input: damage=0 or damage=-5

Processing:
❌ Invalid damage value (must be > 0)
❌ Input ignored, no changes made
⚠️ Warning logged: "Invalid damage value - ignoring"

Result: No changes to production counts
```

## 🔍 Technical Implementation Details

### **Damage Processing Logic**
```cpp
// Determine current shift
bool current_is_day = isDayShift();

// Subtract from appropriate count
if (current_is_day) {
    day_count = max(0, day_count - new_damage);  // Day shift
} else {
    night_count = max(0, night_count - new_damage);  // Night shift
}

// Update total
total_count = day_count + night_count;

// Send with damage_dash for timeseries
String payload = buildCompleteTelemetryPayload("damage_dash", String(new_damage));
```

### **Shift Detection**
- **Day Shift**: 6:00 AM to 6:00 PM (6-18 hours)
- **Night Shift**: 6:00 PM to 6:00 AM (18-6 hours)
- Based on real-time clock synchronization

### **Data Sent to Dashboard**
Every damage event sends complete telemetry including:
- `damage_dash`: Damage amount (for timeseries widget)
- `day_count`: Updated day shift count
- `night_count`: Updated night shift count  
- `total_count`: Updated total production count
- `total_for_hour`: Current hour production
- All user input variables (_dash versions)
- `timestamp`: Current time

### **Error Handling**
- **Negative Prevention**: Uses `max(0, count - damage)` to prevent negative counts
- **Input Validation**: Only processes damage > 0
- **Connection Handling**: Queues data if not connected to ThingsBoard
- **NVS Backup**: Immediately saves changes to prevent data loss

## 🚨 Important Notes

### **Dashboard Setup Required**
1. Create "damage" shared attribute widget for user input
2. Create "damage_dash" timeseries widget for damage tracking
3. Ensure proper widget configuration for numeric input

### **Shift Timing Considerations**
- Damage is applied to the shift during which it was input
- If input at 11:50 PM, it affects night shift counts
- If input at 10:30 AM, it affects day shift counts
- Real-time shift detection ensures accuracy

### **Data Persistence**
- All damage events are immediately saved to NVS
- Production counts are preserved across power cycles
- No data loss during device restart

## 📈 Benefits Achieved

✅ **Real-time Damage Tracking**: Immediate count adjustments  
✅ **Shift-aware Processing**: Automatically detects and adjusts correct shift  
✅ **Timeseries Visualization**: Complete damage history in dashboard  
✅ **Data Integrity**: Prevents negative counts and data corruption  
✅ **Comprehensive Logging**: Detailed debug output for troubleshooting  
✅ **Industrial Reliability**: NVS storage ensures data persistence  

## 🔧 Dashboard Integration

### **Required Widgets**
1. **Input Widget**: "damage" (numeric input for user)
2. **Timeseries Widget**: "damage_dash" (tracks damage over time)
3. **Display Widgets**: Shows updated production counts

### **Data Flow**
```
User Input (Dashboard) → "damage" attribute → ESP32 Processing → 
Updated Counts + "damage_dash" → Dashboard Display + Timeseries
```

This implementation provides a complete, industrial-grade damage tracking system with real-time processing, comprehensive logging, and robust data management.
