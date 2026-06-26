# DCC Codebase Analysis - Comprehensive Overview

## Executive Summary

The DCC (Distributed Congestion Control) system is a C++17-based real-time congestion management system for vehicular wireless networks. It monitors channel congestion via Channel Busy Ratio (CBR) metrics and dynamically adjusts transmission parameters (power, rate, and transmission intervals) to maintain optimal network performance.

**Core Technology**: Linux Netlink API (nl80211) for direct hardware control, POSIX threads with real-time scheduling (SCHED_FIFO priority 70), and atomic operations for thread-safe IPC.

---

## Project Architecture

### Overall System Flow

```
┌─────────────────────────────────────────────────────────────┐
│                      main() Entry Point                      │
│  • Signal handling (SIGINT for graceful shutdown)            │
│  • Command-line argument parsing (2 interfaces required)     │
│  • DccManager instantiation and lifecycle management         │
└────────────────┬────────────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────────────────────────┐
│                 DccManager Worker Thread                     │
│  • Real-time thread (SCHED_FIFO, priority 70)               │
│  • 1000ms polling cycle for CBR updates                      │
│  • State machine logic (5 congestion states)                 │
│  • Cross-layer UDP notifications                            │
└────────────────┬────────────────────────────────────────────┘
                 │
        ┌────────┼────────┐
        │        │        │
        ▼        ▼        ▼
    ┌───────┐ ┌────────┐ ┌─────────────┐
    │CBRRead│ │TxPower │ │RateManager  │
    │er     │ │Manager │ │             │
    └───────┘ └────────┘ └─────────────┘
```

---

## Core Components

### 1. **main.cpp** - Entry Point & Signal Handling
**Purpose**: Application bootstrap and lifecycle management

**Key Features**:
- **Signal Handler**: Implements `SIGINT` (Ctrl+C) handling via `std::signal()` for graceful shutdown
- **Atomic Control**: Uses `std::atomic<bool> keep_running` to safely break the main event loop
- **Parameter Validation**: Requires exactly 2 network interface arguments:
  - Standard network interface (for CBR reading): `wlan0`
  - Radiotap interface (for rate control): `wlan1`
- **Monitoring Output**: Continuously displays:
  - Current CBR percentage
  - Current DCC state
  - 1-second update interval

**Execution Flow**:
```cpp
1. Parse command-line arguments
2. Register SIGINT handler
3. Instantiate DccManager
4. Start DccManager thread (real-time priority)
5. Main loop: Display metrics every 1s until Ctrl+C pressed
6. Graceful shutdown: Call dcc_manager.stop() and cleanup
```

---

### 2. **DccManager** - Core Congestion Control Engine
**Files**: `dcc_manager.h`, `dcc_manager.cpp`

**Purpose**: Central orchestration of congestion detection and adaptive control

**Key Responsibilities**:
- **State Machine**: Maps CBR thresholds to 5 DCC states
- **Cross-Layer Communication**: Sends UDP notifications to CAM process on port 5555 (127.0.0.1)
- **Hardware Adaptation**: Coordinates changes to TX power, modulation/coding scheme (MCS/rate), and transmission intervals
- **Real-Time Scheduling**: Uses `pthread_setschedparam()` to set SCHED_FIFO with priority 70

**DCC States & Thresholds** (based on CBR percentage):

| State | CBR Range | TX Power (dBm) | MCS/Rate (Mbps) | Interval (ms) | Max Frequency |
|-------|-----------|---------------|-----------------|---------------|--------------|
| RELAXED | 0-30% | 23 | 12 | 50 | 20 Hz |
| ACTIVE_1 | 30-40% | 19 | 12 | 100 | 10 Hz |
| ACTIVE_2 | 40-50% | 16 | 18 | 200 | 5 Hz |
| ACTIVE_3 | 50-65% | 13 | 24 | 400 | 2.5 Hz |
| RESTRICTIVE | >65% | 10 | 24 | 1000 | 1 Hz |

**State Transition Logic**:
```cpp
if (cbr_percent < 30%)        → RELAXED
else if (cbr_percent < 40%)   → ACTIVE_1
else if (cbr_percent < 50%)   → ACTIVE_2
else if (cbr_percent < 65%)   → ACTIVE_3
else                           → RESTRICTIVE
```

**Cross-Layer IPC**:
- Uses UDP socket (AF_INET, SOCK_DGRAM)
- Sends new DCC state as 32-bit integer to `127.0.0.1:5555`
- Allows CAM/higher-layer applications to react to congestion changes

**Public API**:
```cpp
void start()                          // Start worker thread with RT priority
void stop()                           // Signal thread to stop and join
float get_current_cbr() const         // Atomic read of CBR percentage
DccState get_current_state() const    // Atomic read of current state
int get_current_interval_ms() const   // Atomic read of TX interval
```

---

### 3. **CBRReader** - Channel Busy Ratio Acquisition
**Files**: `CBRReader.h`, `CBRReader.cpp`

**Purpose**: Monitor wireless channel utilization via Netlink (nl80211)

**Key Features**:
- **Netlink-Based**: Uses Linux kernel's nl80211 generic netlink family
- **Command**: `NL80211_CMD_GET_SURVEY` to query channel statistics
- **Thread-Safe**: Uses `std::mutex` and `std::lock_guard` for synchronized access
- **Callback-Based**: `cbr_handler()` callback processes Netlink responses

**CBR Calculation** (internal to kernel, extracted by CBRReader):
- Channel Busy Ratio = (total_busy_time / total_time) * 100%
  - `total_busy_time` = TX time + RX time + interference time
  - Normalized to 0-1.0 scale internally, converted to percentage by DccManager

**Data Structure** (private CBRData):
```cpp
bool firstTime               // Flag for first-time initialization
bool verbose                 // Debug output control
unsigned long long startActiveTime    // Timestamp tracking
unsigned long long startBusyTime      // Timestamp tracking
unsigned long long startReceiveTime   // Timestamp tracking
unsigned long long startTransmitTime  // Timestamp tracking
float currentCBR             // Latest CBR value (0.0-1.0)
float currentTxTime          // TX duration percentage
float currentRxTime          // RX duration percentage
float currentBusyTime        // Total busy percentage
```

**Thread Safety**:
- Private `m_mutex` protects `m_cbr_data` from race conditions
- Getter methods use `std::lock_guard` for synchronization
- Callback handler acquires lock before modifying data

**Netlink Message Flow**:
1. Allocate Netlink message with `nlmsg_alloc()`
2. Populate with `genlmsg_put()` for nl80211_id
3. Attach interface index with `nla_put_u32()`
4. Register callback handler via `nl_socket_modify_cb()`
5. Send query with `nl_send_auto_complete()`
6. Receive and parse response via `nl_recvmsgs_default()`

**Public API**:
```cpp
void setupCBRReader(bool verbose, std::string interface)  // Initialize
float get_current_cbr()              // Get CBR (0.0-1.0)
float get_current_tx_time()          // Get TX time %
float get_current_rx_time()          // Get RX time %
float get_current_busy_time()        // Get busy time %
void wrapper_start_reading_cbr()     // Trigger CBR read in new thread
```

---

### 4. **TxPowerManager** - Transmission Power Control
**Files**: `TxPowerManager.h`, `TxPowerManager.cpp`

**Purpose**: Dynamically adjust wireless transmit power levels

**Implementation Method**: Netlink nl80211 API (no system calls)

**Key Algorithm**:
```cpp
1. Allocate Netlink socket and connect to generic netlink
2. Resolve nl80211 family ID from kernel
3. Get network interface index from name (if_nametoindex)
4. Create message with NL80211_CMD_SET_WIPHY command
5. Set attributes:
   - NL80211_ATTR_IFINDEX: interface ID
   - NL80211_ATTR_WIPHY_TX_POWER_SETTING: NL80211_TX_POWER_FIXED
   - NL80211_ATTR_WIPHY_TX_POWER_LEVEL: power_dbm * 100  (in mBm units)
6. Send and wait for ACK from kernel
```

**Power Mapping** (DCC to Hardware):
- Input: dBm value (10-23 dBm range)
- Kernel expects: mBm (milli-Bel-milliwatts) = dBm × 100
- Example: 23 dBm → 2300 mBm

**Error Handling**:
- Validates socket allocation
- Checks Netlink connection
- Resolves nl80211 driver ID
- Verifies interface existence
- Returns `false` on failure

**Public API**:
```cpp
bool set_tx_power(const std::string& interface_name, int power_dbm)
```

---

### 5. **RateManager** - Modulation & Coding Scheme (MCS) Control
**Files**: `rate_manager.h`, `rate_manager.cpp`

**Purpose**: Set fixed transmission rates for congestion control

**Dual Implementation**:

**Method 1: `set_fixed_rate()` - System Call Approach**
```cpp
// Executes: sudo iw dev <interface> set bitrates legacy-5 <mbps>
// Example: sudo iw dev wlan1 set bitrates legacy-5 12
```
- Requires elevated privileges
- Simpler but less direct control

**Method 2: `set_fixed_rate_netlink()` - Direct Netlink Approach** (Preferred)
```cpp
1. Interface name → if_index (via if_nametoindex)
2. Allocate and connect Netlink socket
3. Resolve nl80211 driver ID
4. Create message with NL80211_CMD_SET_TX_BITRATE_MASK
5. Set bitrate mask attributes for legacy rates
6. Send to kernel
```

**Rate Mapping** (DCC indices to Mbps):
| Rate Index | Bitrate (Mbps) | Use Case |
|------------|----------------|----------|
| 24 | 24 | Lowest/most restrictive states (ACTIVE_3, RESTRICTIVE) |
| 18 | 18 | ACTIVE_2 (moderate congestion) |
| 12 | 12 | ACTIVE_1, RELAXED (low congestion) |
| 6 | 6 | Fallback (not currently used in state machine) |

**DCC Usage Pattern**:
```cpp
if (new_state == RELAXED)     → rate_manager.set_fixed_rate_netlink(12)
if (new_state == ACTIVE_1)    → rate_manager.set_fixed_rate_netlink(12)
if (new_state == ACTIVE_2)    → rate_manager.set_fixed_rate_netlink(18)
if (new_state == ACTIVE_3)    → rate_manager.set_fixed_rate_netlink(24)
if (new_state == RESTRICTIVE) → rate_manager.set_fixed_rate_netlink(24)
```

**Public API**:
```cpp
bool setup_rate_manager(const std::string& interface)  // Initialize
void set_fixed_rate(int rate_idx)                      // Via system call
void set_fixed_rate_netlink(int rate_idx)              // Via Netlink (preferred)
void cleanup()                                          // Cleanup resources
```

---

### 6. **vehicleDataDef.h** - Data Structure Definitions
**Purpose**: Define vehicle and network entity data models

**Key Structures**:

#### **OptionalDataItem Template**
```cpp
template <class T> class OptionalDataItem
```
- Stores data that may or may not be available
- `m_available`: Boolean flag indicating data validity
- Methods: `getData()`, `isAvailable()`, `setData()`
- Used for optional vehicle parameters (width, length, etc.)

#### **StationType Enumeration**
```cpp
enum StationTypeLDM {
    StationType_LDM_pedestrian = 1,
    StationType_LDM_cyclist = 2,
    StationType_LDM_moped = 3,
    StationType_LDM_motorcycle = 4,
    StationType_LDM_passengerCar = 5,
    StationType_LDM_bus = 6,
    StationType_LDM_lightTruck = 7,
    StationType_LDM_heavyTruck = 8,
    StationType_LDM_trailer = 9,
    StationType_LDM_specialVehicles = 10,
    StationType_LDM_tram = 11,
    StationType_LDM_roadSideUnit = 15,
    // Plus detected variants (100-115)
}
```

#### **vehicleData Structure**
Comprehensive vehicle record containing:
- **Identification**: `stationID`, `macaddr[6]`, `ipaddr`, `publicipaddr`
- **Positioning**: `lat`, `lon`, `elevation`, `heading`
- **Motion**: `speed_ms`, `xSpeed`, `ySpeed`, `xDistance`, `yDistance`
- **Timing**: `gnTimestamp`, `camTimestamp`, `timestamp_us`, `lastCPMincluded`
- **Radio**: `rssi_dBm`
- **Attributes**: `stationType`, `vehicleWidth`, `vehicleLength`
- **Lighting**: `exteriorLights` (bit string)
- **CPM Data**: `perceivedBy`, `confidence`

**Constants**:
```cpp
#define RSSI_UNAVAILABLE -80000
#define LDM_HEADING_UNAVAILABLE 3601.0
#define DEG_2_RAD_AIM(val) ((val) * M_PI / 180.0)
#define vehicleDataVector_t(name) std::vector<ldmmap::vehicleData_t> name
```

---

### 7. **utils.h/cpp** - Utility Functions Library
**Purpose**: Cross-module utility functions for timing, Netlink operations, RSSI queries, and logging

**Key Functions**:

#### **Timestamp Functions**
```cpp
uint64_t get_timestamp_us()              // Monotonic clock microseconds
uint64_t get_timestamp_us_realtime()     // UTC realtime microseconds
uint64_t get_timestamp_ns()              // Monotonic nanoseconds
uint64_t get_timestamp_ms_gn()           // GN (ITS generic) timestamp in ms
uint64_t get_timestamp_ms_cam()          // CAM delta time timestamp
uint64_t get_timestamp_ms_cpm()          // CPM (Collective Perception Message) timestamp
```
- Epoch Reference: ITS epoch = 2004-01-01 00:00:00 UTC
- `TIME_SHIFT_MILLI = 1072915200000` (ms offset from Unix epoch)

#### **Netlink Socket Management**
```cpp
nl_sock_info_t open_nl_socket(std::string interface_name)
// Returns: {nl_sock, ifindex, nl80211_id, sock_valid}

void free_nl_socket(nl_sock_info_t nl_sock_info)
// Cleanup: nl_socket_free() on the nl_sock pointer
```

#### **RSSI (Received Signal Strength Indicator) Queries**
```cpp
double get_rssi_from_iw(uint8_t macaddr[6], std::string interface_name)
// Uses external 'iw' command via popen()

double get_rssi_from_netlink(uint8_t macaddr[6], nl_sock_info_t nl_sock_info)
// Direct Netlink query (preferred)

std::unordered_map<std::string, double> get_all_rssi_from_netlink(nl_sock_info_t nl_sock_info)
// Batch query all visible stations
```

#### **Logging & Formatting**
```cpp
int logfprintf(FILE *stream, std::string modulename, const char *format, ...)
// Formatted logging with timestamp and module name
// Output: "[LOG - <module>] (YYYY-MM-DD HH:MM:SS) <message>"

std::string exteriorLights_bit_to_string(uint8_t extLights)
// Convert light status bits to readable string

std::string doubleToString(double value, int precision = 7)
// Convert double to string with specified precision

bool doublecomp(double d1, double d2, double eps = 0.0001)
// Compare floating-point values with epsilon tolerance
```

#### **Transmission Power Utilities**
```cpp
void setNewTxPower(double txPower, std::string dissemination_interface)
uint32_t getTxPower()
// Legacy power control functions
```

#### **Timer File Descriptors** (for polling-based scheduling)
```cpp
int timer_fd_create(struct pollfd &pollfd, int &clockFd, uint64_t time_us)
// Create timerfd for event-driven I/O scheduling
// Integrates with poll() for efficient waiting
```

---

## Build System

### **Makefile Configuration**

**Compiler Settings**:
```makefile
CC = gcc
CXX = g++
```

**Netlink Integration**:
```makefile
NL_CFLAGS := $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0)
NL_LIBS := $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0)
```

**Compilation Flags**:
- `-Wall`: Enable all warnings
- `-std=c++17`: C++17 standard
- `-IUtilsEncode`: Include UtilsEncode directory
- **Netlink flags** automatically detected via pkg-config

**Linking**:
```makefile
LDFLAGS = -lpthread -lssl -lcrypto -lm $(NL_LIBS)
```
- `pthread`: POSIX threads
- `ssl`, `crypto`: OpenSSL libraries
- `m`: Math library
- Netlink libraries (libnl-3, libnl-genl-3)

**Source Files**:
```makefile
C_SOURCES: UtilsEncode/*.c
CXX_SOURCES: main.cpp, CBRReader.cpp, utils.cpp, TxPowerManager.cpp, 
             dcc_manager.cpp, rate_manager.cpp
```

**Build Targets**:
```bash
make          # Build dcc executable
make clean    # Remove object files and binary
```

---

## Data Flow Diagram

```
┌─────────────────────────────────────────────────────────┐
│  DccManager Worker Loop (1000ms polling)                │
└──────────────────┬──────────────────────────────────────┘
                   │
                   ├─────────────────────┐
                   │                     │
                   ▼                     ▼
        ┌────────────────────┐  ┌───────────────┐
        │ CBRReader          │  │ Read raw CBR  │
        │ (Netlink nl80211)  │  │ value (0-1)   │
        └────────┬───────────┘  └───────────────┘
                 │
                 ▼
        ┌────────────────────────┐
        │ Convert to percentage  │
        │ cbr_percent = raw * 100│
        └────────┬───────────────┘
                 │
                 ▼
        ┌────────────────────────┐
        │ Apply state machine    │
        │ (5 thresholds)         │
        └────────┬───────────────┘
                 │
        ┌────────┴────────┬──────────────────────┐
        │                 │                      │
        ▼                 ▼                      ▼
    ┌────────────┐  ┌─────────────┐  ┌──────────────────┐
    │ TxPower    │  │ RateManager │  │ UDP Cross-Layer  │
    │ Manager    │  │ (Netlink)   │  │ Notification     │
    │ (Netlink)  │  │             │  │ (127.0.0.1:5555) │
    │ 10-23 dBm  │  │ 6-24 Mbps   │  │ → CAM process    │
    └────────────┘  └─────────────┘  └──────────────────┘
         │                │                  │
         └────────────────┴──────────────────┘
                         │
                         ▼
              Hardware state updated
          (Power, Rate, TX Interval)
```

---

## Threading Model

### **Thread Management**:

| Thread | Name | Priority | Role |
|--------|------|----------|------|
| Main | main() | Normal | Event loop monitoring, signal handling |
| DCC Worker | DccManager::dcc_worker_loop() | SCHED_FIFO=70 | Core congestion control logic |
| CBR Reader | CBRReader::start_reading_cbr() | Normal | Netlink communication (spawned per poll cycle) |

### **Synchronization Primitives**:
- `std::atomic<T>`: Atomic variables for CBR, state, interval (lock-free)
- `std::mutex`: Protects CBRReader data structures
- `std::lock_guard`: RAII-based mutex locking
- Signal handler with `std::atomic<bool> keep_running`

### **Real-Time Considerations**:
```cpp
// DccManager sets itself to real-time priority
pthread_t native_handle = dcc_thread_.native_handle();
struct sched_param param;
param.sched_priority = 70;  // RT priority (max 99)
pthread_setschedparam(native_handle, SCHED_FIFO, &param);
```
- Requires `sudo` execution for SCHED_FIFO access
- Prevents other processes from starving the DCC control loop

---

## Execution Requirements

### **Prerequisites**:
1. **Root/Sudo Access**: Required for:
   - Setting real-time thread priority (SCHED_FIFO)
   - Modifying wireless power/rate via Netlink
   - Accessing Netlink nl80211 API

2. **System Dependencies**:
   - Linux kernel ≥ 4.0 (nl80211 support)
   - `libnl-3.0` and `libnl-genl-3.0`
   - OpenSSL development libraries

3. **Hardware**:
   - WiFi adapter supporting nl80211 (modern Intel, Atheros, Realtek chips)
   - Monitor/radiotap mode support

### **Command-Line Usage**:
```bash
sudo ./dcc <interface> <radiotap_interface>
# Example:
sudo ./dcc wlan0 wlan1
```

---

## Key Features Summary

| Feature | Implementation | Benefit |
|---------|-----------------|---------|
| **Real-Time Scheduling** | SCHED_FIFO priority 70 | Predictable latency, immune to system load |
| **Channel Monitoring** | Netlink nl80211 CBR queries | Direct kernel-level access, accurate metrics |
| **Power Adaptation** | Netlink nl80211 commands | Dynamic range 10-23 dBm |
| **Rate Adaptation** | Netlink nl80211 bitrate mask | 6/12/18/24 Mbps selection |
| **Cross-Layer IPC** | UDP socket (127.0.0.1:5555) | Integration with CAM/CPM processes |
| **Graceful Shutdown** | SIGINT handler + atomic flag | Clean resource cleanup |
| **Thread Safety** | Atomics + mutexes | Race-condition free operation |
| **Modular Design** | Separate manager classes | Maintainability, testability |

---

## Summary of Components

### **Files Organization**:
```
├── main.cpp                    # Entry point (70 lines)
├── dcc_manager.cpp/h           # Core logic (250+ lines)
├── CBRReader.cpp/h             # Channel monitoring (200+ lines)
├── TxPowerManager.cpp/h        # Power control (100+ lines)
├── rate_manager.cpp/h          # Rate control (150+ lines)
├── utils.cpp/h                 # Utilities (500+ lines)
├── vehicleDataDef.h            # Data structures
├── Makefile                    # Build configuration
├── README.md                   # Project documentation
└── UtilsEncode/                # Additional encoding utilities
```

### **Total Lines of Code**: ~1,500-2,000 lines (excluding utilities)

### **External Dependencies**:
- `libnl-3.0`: Netlink communication
- `libpthread`: Thread support
- `libssl`, `libcrypto`: Cryptography (OpenSSL)
- `libm`: Math functions

---

## Notable Algorithms

### **1. CBR State Machine**
- **Hysteresis**: Single threshold-based state transitions (no hysteresis gap to prevent oscillation)
- **Update Rate**: Once per second (1000ms cycle)
- **Smooth Transition**: Immediate state change when threshold crossed

### **2. Adaptive Parameter Mapping**
- **Inverse Relationship**: Lower rates (24 Mbps) in higher congestion (more robust)
- **Power Reduction**: 23→10 dBm (13 dBm reduction across 5 states)
- **TX Interval Doubling**: Geometric progression (50ms→100ms→200ms→400ms→1000ms)

### **3. Netlink Protocol Flow**
- **Generic Netlink**: nl80211 family-based command/response
- **Attribute Encoding**: NLA (Netlink Attribute) TLV format
- **Synchronous Operation**: Request-response pattern, blocks until kernel ACK

---

## Safety & Reliability Features

1. **Error Handling**: Graceful degradation on Netlink failures
2. **Resource Cleanup**: RAII-based socket and message deallocation
3. **Timeout Prevention**: No indefinite blocking; controlled wait on Netlink messages
4. **Logging**: Console output for state transitions and errors
5. **Atomic Variables**: Lock-free reads for performance metrics
6. **Thread Cleanup**: Proper join/wait on thread termination

