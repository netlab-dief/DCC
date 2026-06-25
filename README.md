# DCC - Distributed Congestion Control

A C++ implementation of a Distributed Congestion Control (DCC) system for vehicular wireless networks. This project monitors channel congestion and dynamically adjusts transmission parameters to optimize network performance.

## Overview

DCC is a congestion management system that monitors the channel busy ratio (CBR) and adjusts transmission rates and power levels to maintain optimal network conditions. The system operates on wireless interfaces and uses real-time monitoring to implement adaptive congestion control.

## Features

- **Real-time Channel Monitoring**: Continuously monitors Channel Busy Ratio (CBR) using Radiotap interfaces
- **Dynamic State Management**: Transitions between different congestion states (RELAXED, ACTIVE_1, ACTIVE_2, ACTIVE_3, RESTRICTIVE)
- **Transmission Power Control**: Adaptively adjusts transmission power based on network conditions
- **Rate Adaptation**: Manages transmission rates according to current congestion state
- **Thread-safe Operations**: Employs atomic operations and thread synchronization for safe concurrent access

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

| State | Description | Typical CBR Range |
|-------|-------------|-------------------|
| **RELAXED** | Low congestion, normal operation | CBR < 30% |
| **ACTIVE_1** | Mild congestion detected | 30% ≤ CBR < 40% |
| **ACTIVE_2** | Moderate congestion | 40% ≤ CBR < 50% |
| **ACTIVE_3** | High congestion | 50% ≤ CBR < 60% |
| **RESTRICTIVE** | Severe congestion | CBR ≥ 60% |

## Module Descriptions

### DCC Manager (`dcc_manager.h/cpp`)
Coordinates the overall congestion control logic:
- Monitors CBR from the radio interface
- Transitions between states based on congestion thresholds
- Triggers power and rate adjustments
- Manages monitoring thread lifecycle

### CBR Reader (`CBRReader.h/cpp`)
Captures and analyzes radio channel activity:
- Uses Netlink sockets for kernel communication
- Reads packet metadata from Radiotap interface
- Calculates Channel Busy Ratio
- Thread-safe querying of current CBR value

### Transmission Power Manager (`TxPowerManager.h/cpp`)
Manages transmit power adjustments:
- Scales transmission power based on congestion state
- Communicates with wireless driver via netlink
- Maintains power level constraints

### Rate Manager (`rate_manager.h/cpp`)
Adapts transmission rate:
- Adjusts data rate based on network conditions
- Implements rate selection strategy
- Maintains minimum QoS requirements

### Utilities (`utils.h/cpp`)
Common helper functions for:
- Data conversion and encoding
- Logging and debugging
- Network utilities

## API Reference

### DccManager Class

#### Constructor
```cpp
DccManager(std::string interface, std::string interface_radiotap, 
           int initial_interval_ms = 100);
```

#### Methods
- `void start()` - Start the monitoring thread
- `void stop()` - Stop the monitoring thread and cleanup
- `float get_current_cbr() const` - Get current Channel Busy Ratio
- `DccState get_current_state() const` - Get current DCC state

## Thread Safety

The implementation uses:
- `std::atomic<bool>` for atomic flag operations
- Mutex protection for shared data structures
- POSIX thread synchronization primitives
- Signal handlers for graceful shutdown

## Logging

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
