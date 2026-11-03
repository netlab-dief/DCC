# Compiler settings
CC = gcc
CXX = g++

# Netlink flags necessari per CBRReader
NL_CFLAGS := $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0 2>/dev/null || echo -I/usr/include/libnl3)
NL_LIBS   := $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0 2>/dev/null || echo -lnl-3 -lnl-genl-3)

# Gli header ASN.1 sono inclusi anche dai sorgenti C++ del generatore CAM.
CFLAGS = -Wall -Icam/UtilsEncode -MMD -MP
CXXFLAGS = -Wall -Icam/UtilsEncode $(NL_CFLAGS) -std=c++17 -MMD -MP
LDFLAGS = -lpthread -lssl -lcrypto -lm $(NL_LIBS)

# Directories
C_DIR = cam/UtilsEncode
CXX_DIR = .
OUTPUT_DIR = .

# C source files
C_SOURCES := $(wildcard $(C_DIR)/*.c)

# C++ source files (AGGIUNTO dcc/utils.cpp alla lista)
CXX_SOURCES := main.cpp CBRReader.cpp utils.cpp TxPowerManager.cpp dcc_manager.cpp rate_manager.cpp dcc_state_machine.cpp airtime_gate.cpp gate_server.cpp cbr_sampler.cpp cam/cam_message.cpp cam/cam_sender.cpp

# Object files
C_OBJS := $(patsubst $(C_DIR)/%.c, $(C_DIR)/%.o, $(C_SOURCES))
CXX_OBJS := $(patsubst %.cpp, %.o, $(CXX_SOURCES))

# Output binary in the main folder
TARGET = dcc

.DEFAULT_GOAL := all
-include $(C_OBJS:.o=.d) $(CXX_OBJS:.o=.d)

# Default rule
all: $(TARGET)

# Compile C files
$(C_DIR)/%.o: $(C_DIR)/%.c
	$(CC) $(CFLAGS) -o $@ -c $<

# Compile C++ files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $<

# Link everything, placing the final binary in the main folder
$(TARGET): $(C_OBJS) $(CXX_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

# Clean up object files and executable
clean:
	rm -f $(C_OBJS) $(CXX_OBJS) $(C_OBJS:.o=.d) $(CXX_OBJS:.o=.d) $(TARGET) tests/test_dcc_state_machine tests/test_dcc_control tests/test_cbr_sampler tests/test_cam tests/test_console_log

.PHONY: all clean test test-cpp test-python

test: test-cpp test-python

test-cpp: tests/test_dcc_state_machine tests/test_dcc_control tests/test_cbr_sampler tests/test_cam tests/test_console_log
	./tests/test_dcc_state_machine
	./tests/test_dcc_control
	./tests/test_cbr_sampler
	./tests/test_cam
	./tests/test_console_log

tests/test_console_log: tests/test_console_log.cpp console_log.h
	$(CXX) -Wall -Wextra -Werror -std=c++17 -pthread -I. tests/test_console_log.cpp -o $@

tests/test_cam: tests/test_cam.cpp cam/cam_message.cpp cam/cam_message.h cam/cam_sender.cpp cam/cam_sender.h cam/cam_timing.h cam/cam_generation.h cam/cam_telemetry.h cam/airtime_estimator.h console_log.h airtime_gate.cpp airtime_gate.h $(C_OBJS)
	$(CXX) -Wall -Wextra -Werror -std=c++17 -pthread -I. -Icam/UtilsEncode tests/test_cam.cpp cam/cam_message.cpp cam/cam_sender.cpp airtime_gate.cpp $(C_OBJS) -o $@

tests/test_dcc_state_machine: tests/test_dcc_state_machine.cpp dcc_state_machine.cpp dcc_state_machine.h
	$(CXX) -Wall -Wextra -Werror -std=c++17 -I. tests/test_dcc_state_machine.cpp dcc_state_machine.cpp -o $@

test-python:
	python3 -m unittest discover -s tests -p 'test_*.py' -v

tests/test_dcc_control: tests/test_dcc_control.cpp dcc_adaptive.h airtime_gate.cpp airtime_gate.h gate_server.cpp gate_server.h
	$(CXX) -Wall -Wextra -Werror -std=c++17 -pthread -I. tests/test_dcc_control.cpp airtime_gate.cpp gate_server.cpp -o $@

tests/test_cbr_sampler: tests/test_cbr_sampler.cpp cbr_sampler.cpp cbr_sampler.h dcc_adaptive.h
	$(CXX) -Wall -Wextra -Werror -std=c++17 -pthread -I. tests/test_cbr_sampler.cpp cbr_sampler.cpp -o $@
