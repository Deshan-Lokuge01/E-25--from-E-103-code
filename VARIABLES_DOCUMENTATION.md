# IoT Industrial Machine - Variable Documentation

## Overview
This document explains all the variables that are passed between the ESP32 and ThingsBoard dashboard for the 4-mould industrial machine monitoring system.

## Machine Configuration
- **MOULDS**: 4 (defined as constant)
- **Operation Time Tolerance**: ±5 seconds for valid cycle detection
- **Accidental Press Protection**: 5 seconds minimum between valid presses

## Variables Passed FROM ESP32 TO Dashboard (Telemetry Data)

### Core Production Counters
1. **total_count** (int)
   - Description: Overall cumulative count since system start (never resets)
   - Updates: Incremented by (moulds * quantity) on each valid button press
   - Reset: Never

2. **day_count** (int)
   - Description: Current day shift count (6 AM - 6 PM)
   - Updates: Incremented by active moulds count during day shift
   - Reset: Daily at 6:00 AM

3. **night_count** (int)
   - Description: Current night shift count (6 PM - 6 AM)
   - Updates: Incremented by active moulds count during night shift
   - Reset: Daily at 6:00 AM

4. **today_total** (int)
   - Description: Today's total production (day_count + night_count)
   - Updates: Calculated automatically
   - Reset: Daily at 6:00 AM

### Dashboard Echo Variables (For Widget Compatibility)
5. **day_dash_assignee** (string)
   - Description: Echo of day assignee back to dashboard for History widget
   - Purpose: Allows string data to be passed between widgets
   - Updates: When day_assignee is updated via dashboard

6. **night_dash_assignee** (string)
   - Description: Echo of night assignee back to dashboard for History widget
   - Purpose: Allows string data to be passed between widgets
   - Updates: When night_assignee is updated via dashboard

7. **comments_dash** (string)
   - Description: Echo of comments back to dashboard for History widget
   - Purpose: Allows string data to be passed between widgets
   - Updates: When comments are updated via dashboard

8. **target_dash** (int)
   - Description: Echo of target back to dashboard for widgets
   - Purpose: Allows target data to be passed between widgets
   - Updates: When target is updated via dashboard

### Status and Metadata
9. **device_active** (boolean)
   - Description: Indicates if device is connected to ThingsBoard
   - Updates: Real-time based on connection status

10. **changed** (string)
    - Description: Indicates if corrections were applied
    - Values: "No", "Yes - Day Correction Applied", "Yes - Night Correction Applied"
    - Updates: When corrections are applied via dashboard

11. **date** (string)
    - Description: Current date in DD/MM/YYYY format
    - Updates: Real-time, used for daily tracking

12. **timestamp** (string)
    - Description: Timestamp of last button press in YYYY-MM-DD HH:MM:SS format
    - Updates: On each valid button press

13. **current_shift** (string)
    - Description: Current active shift
    - Values: "Day" or "Night"
    - Updates: Real-time based on time (6 AM = Day, 6 PM = Night)

14. **active_assignee** (string)
    - Description: Currently active shift assignee
    - Updates: Real-time based on current shift

## Variables Received FROM Dashboard TO ESP32 (RPC Commands)

### Production Settings
15. **moulds** (int, range: 1-4)
    - RPC Method: "setMoulds"
    - Description: Number of active moulds operating
    - Effect: Multiplies button press count (e.g., if moulds=3, each press adds 3 to counters)
    - Default: 4

16. **op_time** (int, in seconds)
    - RPC Method: "setOpTime"
    - Description: Expected operation cycle time
    - Effect: Used to validate button presses (must be within op_time ± 5 seconds)
    - Default: 300 seconds (5 minutes)

17. **target** (int)
    - RPC Method: "setTarget"
    - Description: Daily production target
    - Effect: Stored and echoed back as target_dash

### Assignee Management
18. **day_assignee** (string)
    - RPC Method: "setDayAssignee"
    - Description: Day shift operator name
    - Effect: Stored and echoed back as day_dash_assignee

19. **night_assignee** (string)
    - RPC Method: "setNightAssignee"
    - Description: Night shift operator name
    - Effect: Stored and echoed back as night_dash_assignee

20. **comments** (string)
    - RPC Method: "setComments"
    - Description: General comments or notes
    - Effect: Stored and echoed back as comments_dash

### Correction System
21. **day_correction** (int, can be positive or negative)
    - RPC Method: "setDayCorrection"
    - Description: Correction value for day count
    - Effect: Added to day_count, updates total_count, sets changed="Yes - Day Correction Applied"

22. **night_correction** (int, can be positive or negative)
    - RPC Method: "setNightCorrection"
    - Description: Correction value for night count
    - Effect: Added to night_count, updates total_count, sets changed="Yes - Night Correction Applied"

## Button Press Validation Logic

### Valid Press Criteria (ALL must be met):
1. **Connection Check**: Device must be connected to ThingsBoard
2. **Accidental Press Protection**: Minimum 5 seconds since last valid press
3. **Cycle Time Validation**: Time since previous press must be within (op_time - 5 seconds) to (op_time + 5 seconds)

### When Valid Press Occurs:
1. total_count += moulds
2. If day shift: day_count += moulds
3. If night shift: night_count += moulds
4. timestamp updated to current time
5. All data saved to NVS (non-volatile storage)
6. Telemetry sent to dashboard

## Data Persistence
- All counters and settings are saved to ESP32 NVS (Non-Volatile Storage)
- Data survives power cycles and reboots
- Daily reset occurs at 6:00 AM

## Real-time Updates
- Heartbeat every 2 seconds with current status
- Immediate telemetry on button press
- Immediate response to dashboard RPC commands

## Example Dashboard RPC Commands

```json
// Set active moulds to 3
{"method": "setMoulds", "params": {"value": 3}}

// Set day assignee
{"method": "setDayAssignee", "params": {"value": "John Doe"}}

// Apply day correction (subtract 5)
{"method": "setDayCorrection", "params": {"value": -5}}

// Set operation time to 4 minutes
{"method": "setOpTime", "params": {"value": 240}}
```

## Widget Data Flow
1. User inputs → Dashboard RPC → ESP32
2. ESP32 processes and stores data
3. ESP32 echoes data back → Dashboard (as _dash variables)
4. Dashboard widgets can now access string data via _dash variables
5. Historical data tracking enabled through echoed variables
