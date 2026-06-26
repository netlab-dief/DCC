# DCC - Distributed Congestion Control

A C++ implementation of a Distributed Congestion Control (DCC) system for vehicular wireless networks. This project monitors channel congestion and dynamically adjusts transmission parameters to optimize network performance.

## Overview

DCC is a congestion management system that monitors the channel busy ratio (CBR) and adjusts transmission rates and power levels to maintain optimal network conditions. The system operates on wireless interfaces and uses real-time monitoring to implement adaptive congestion control.

## Features

- **Real-time Channel Monitoring**: Continuously monitors Channel Busy Ratio (CBR) using Netlink-based Radiotap interfaces
- **5-State Congestion Machine**: Dynamic transitions between RELAXED, ACTIVE_1, ACTIVE_2, ACTIVE_3, and RESTRICTIVE states based on CBR thresholds
- **Adaptive Transmission Power Control**: Scales TX power from 23 dBm (relaxed) to 10 dBm (restrictive) for optimal channel efficiency
- **Intelligent Rate Adaptation**: Adjusts data rates (6-24 Mbps) and transmission intervals (50ms-1s) based on network congestion
- **Real-time Scheduling**: Operates with SCHED_FIFO priority 70 for deterministic, low-latency congestion control
- **Cross-Layer IPC**: Sends UDP notifications to CAM process (127.0.0.1:5555) for inter-process communication
- **Thread-safe Operations**: Employs atomic operations, mutex protection, and POSIX synchronization primitives
- **Graceful Shutdown**: Clean SIGINT handler with proper resource cleanup
- **Comprehensive Logging**: Detailed state transitions, CBR readings, and adjustment notifications

## Project Structure

```
├── main.cpp                 # Main entry point and signal handling
├── dcc_manager.cpp/.h       # Core DCC management logic
├── CBRReader.cpp/.h         # Channel Busy Ratio reader (Netlink-based)
├── TxPowerManager.cpp/.h    # Transmission power management
├── rate_manager.cpp/.h      # Rate adaptation management
├── utils.cpp/.h             # Utility functions
├── vehicleDataDef.h         # Vehicle data structure definitions
├── Makefile                 # Build configuration
└── UtilsEncode/             # Additional utilities and encoding support
```

## Prerequisites

- **C++17** or later
- **Linux kernel** with wireless extensions support
- **libnl-3.0** and **libnl-genl-3.0** (for Netlink communication)
- **OpenSSL** development libraries
- **GCC/G++** compiler
- **POSIX threads** support

### Installation of Dependencies

On Ubuntu/Debian:
```bash
sudo apt-get install libssl-dev libnl-3-dev libnl-genl-3-dev build-essential
```

On Fedora/RHEL:
```bash
sudo dnf install openssl-devel libnl3-devel gcc-c++
```

## Building

The project uses a Makefile for compilation. Build the project with:

```bash
make
```

To clean build artifacts:
```bash
make clean
```

To rebuild from scratch:
```bash
make rebuild
```

## Usage

Run the DCC system with two wireless interfaces:

```bash
sudo ./dcc <standard_interface> <radiotap_interface>
```

### Example

```bash
sudo ./dcc wlan0 wlan1
```

Where:
- `wlan0` is the standard network interface for normal wireless operations
- `wlan1` is the Radiotap interface for advanced packet monitoring

### Stopping the Application

Press **Ctrl+C** to gracefully shut down the system:

```
^C
[MAIN] Ricevuto Ctrl+C (SIGINT). Avvio chiusura pulita...
```

## DCC States

The system operates in five different states based on channel congestion:

| State | CBR Range | TX Power | TX Rate | TX Interval | Max Frequency |
|-------|-----------|----------|---------|-------------|---------------|
| **RELAXED** | < 30% | 23 dBm | 12 Mbps | 50 ms | 20 Hz |
| **ACTIVE_1** | 30-40% | 19 dBm | 12 Mbps | 100 ms | 10 Hz |
| **ACTIVE_2** | 40-50% | 16 dBm | 18 Mbps | 200 ms | 5 Hz |
| **ACTIVE_3** | 50-65% | 13 dBm | 24 Mbps | 400 ms | 2.5 Hz |
| **RESTRICTIVE** | ≥ 65% | 10 dBm | 24 Mbps | 1000 ms | 1 Hz |

## Module Descriptions

### DCC Manager (`dcc_manager.h/cpp`)
Core congestion control engine running as a real-time thread:
- Runs with SCHED_FIFO scheduling at priority 70 for deterministic behavior
- Polls every 1000ms for CBR updates from the kernel
- Implements 5-state machine with automatic state transitions
- Triggers synchronized power and rate adjustments
- Sends UDP notifications (127.0.0.1:5555) for inter-process communication
- Manages monitoring thread lifecycle and graceful shutdown
Netlink-based channel monitoring system:
- Queries kernel via `NL80211_CMD_GET_SURVEY` command
- Calculates Channel Busy Ratio = (busy_time / total_time) from wireless survey data
- Integrates directly with Linux wireless extensions (nl80211)
- Thread-safe with mutex protection for concurrent access
- Provides real-time channel state information to the DCC managertion
- Reads packet metadata from Radiotap interface
Direct hardware transmission power control:
- Sets TX power levels from 10-23 dBm via Netlink nl80211
- Uses `NL80211_CMD_SET_TX_POWER` for direct kernel control (no system calls)
- Dynamically scales power based on DCC congestion state
- Optimizes transmit efficiency while managing channel utilizationPowerManager.h/cpp`)
Manages transmit power adjustments:
Modulation and Coding Scheme (MCS) adaptation:
- Sets fixed transmission rates: 6, 12, 18, 24 Mbps
- Uses Netlink nl80211 `NL80211_CMD_SET_TX_BITRATE_MASK` for rate control
- Higher rates (24 Mbps) applied in more restrictive states for robustness
- Adapts rates based on real-time channel congestion assessment
### Rate Manager (`rate_manager.h/cpp`)
Adapts transmission rate:
- Adjusts data rate based on network conditions
- Implements rate selection strategy
- Mprehensive utility library providing:
- Timestamp functions (monotonic, realtime, GN, CAM, CPM epochs)
- Netlink socket creation and management
- RSSI (signal strength) queries and monitoring
- Logging and formatted output utilities
- Timer file descriptor creation for scheduling

### Vehicle Data Definitions (`vehicleDataDef.h`)
Data structures for vehicular network communication:
- `OptionalDataItem<T>`: Template for nullable/optional data fields
- `vehicleData`: Complete vehicle record with position, speed, RSSI, and timestamps
- `StationTypeLDM`: Vehicle classification enumeration (pedestrian, cyclist, car, truck, RSU, etc.)ions for:
- Data conversion and encoding
- Logging and debugging
- Network utilities

## API Reference

### DccManager Class

#### Constructor
```cpp
DccManager(std:: & Real-time Characteristics

The implementation employs multiple synchronization mechanisms:
- **Atomic Operations**: `std::atomic<bool>` for lock-free flag operations
- **Mutex Protection**: Synchronized access to shared data structures
- **POSIX Primitives**: Thread synchronization and condition variables
- **Signal Handlers**: SIGINT handler for graceful shutdown with cleanup
- **Real-time Scheduling**: SCHED_FIFO priority 70 for deterministic latency
- **Non-bl & Monitoring

Real-time system monitoring and diagnostics:
- State transitions logged with CBR values and thresholds
- Periodic CBR readings displayed (every monitoring cycle)
- Power and rate adjustments reported with dBm and Mbps values
- Error conditions clearly marked with context
- 1-second monitoring output for visibility into system behavior

Example output:
```
[MAIN] Avvio del sistema di controllo DCC...
[DCC] Current state: ACTIVE_1, CBR: 35.2%
[POWER] Adjusting Tx power to 19 dBm
[RATE] Rate adjusted to 12 Mbps
[DCC] Notification sent to CAM process
^C
[MAIN] Ricevuto Ctrl+C (SIGINT). Avvio chiusura pulita...
[DCC] Cleanup completed
The system provides console output for monitoring:
- State transitions are logged
- CBR readings are displayed periodically
- Power and rate adjustments are reported
- Error conditions are clearly marked

Example output:
```
[MAIN] Avvio del sistema di controllo DCC...
[DCC] Current state: ACTIVE_1, CBR: 35.2%
[POWER] Adjusting Tx power to -10 dBm
[RATE] Rate adjusted to 18 Mbps
```

## Troubleshooting

### Permission Denied
The program requires root privileges to access wireless interfaces:
```bash
sudo ./dcc wlan0 wlan1
```

### Interface Not Found
Verify available interfaces:
```bash
iwconfig
# or
iw dev
```

### Netlink Errors
Ensure libnl libraries are properly installed and can be linked:
```bash
pkg-config --cflags --libs libnl-3.0 libnl-genl-3.0
```

## Performance Considerations

- Monitoring thread runs at configurable intervals (default: 100ms)
- Real-time metric calculations based on recent packet activity
- Minimal overhead for state transitions
- Efficient atomic operations for lock-free checks

## Contributing

When contributing to this project:
1. Follow the existing code style (C++17)
2. Add thread-safety considerations for any new shared data
3. Test with various wireless drivers
4. Document new functionality

## License

[Add appropriate license information]

## Authors

- netlab-dief team

## References

- IEEE 802.11 Wireless Standards
- Linux Netlink Protocol Documentation
- Wireless Extensions and nl80211 Driver Interface

---

For more information and issues, please refer to the project repository.
