# Compilers and platform dependencies.
CC = gcc
CXX = g++
NL_CFLAGS := $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0 2>/dev/null || echo -I/usr/include/libnl3)
NL_LIBS := $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0 2>/dev/null || echo -lnl-3 -lnl-genl-3)

# Forwarding headers preserve CAM's historical relative includes of root headers.
COMPAT_HEADERS := build/include/console_log.h build/include/airtime_gate.h
PROJECT_INCLUDES := -I. -Isrc -Ibuild/include/cam -Icam/UtilsEncode
CPPFLAGS += $(PROJECT_INCLUDES)
CFLAGS = -Wall -MMD -MP
CXXFLAGS = -Wall $(NL_CFLAGS) -std=c++17 -MMD -MP
LDLIBS = -lpthread -lssl -lcrypto -lm $(NL_LIBS)
TEST_FLAGS = -Wall -Wextra -Werror -std=c++17 -pthread

C_SOURCES := $(wildcard cam/UtilsEncode/*.c)
CXX_SOURCES := src/app/main.cpp src/radio/CBRReader.cpp src/common/utils.cpp \
    src/radio/TxPowerManager.cpp src/dcc/dcc_manager.cpp src/radio/rate_manager.cpp \
    src/dcc/dcc_state_machine.cpp src/dcc/airtime_gate.cpp src/radio/cbr_sampler.cpp \
    src/net/dcc_net.cpp src/net/gn_receiver.cpp src/net/dcc_network.cpp \
    src/dcc/dcc_access.cpp src/dcc/dcc_cross.cpp src/radio/dcc_radio.cpp \
    cam/cam_message.cpp cam/cam_sender.cpp
C_OBJS := $(patsubst %.c,build/%.o,$(C_SOURCES))
CXX_OBJS := $(patsubst %.cpp,build/%.o,$(CXX_SOURCES))
PROJECT_HEADERS := $(wildcard src/*/*.h cam/*.h)
TARGET = dcc
TEST_NAMES := test_dcc_state_machine test_dcc_control test_cbr_sampler test_cam test_console_log test_dcc_net test_dcc_architecture test_dcc_manager
ifeq ($(shell uname -s),Linux)
TEST_NAMES += test_radio_netlink
endif
TEST_BINS := $(addprefix build/tests/,$(TEST_NAMES))

.DEFAULT_GOAL := all
.PHONY: all clean test test-cpp test-python
-include $(C_OBJS:.o=.d) $(CXX_OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(C_OBJS) $(CXX_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/%.o: %.cpp | $(COMPAT_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/include/console_log.h:
	@mkdir -p build/include/cam
	@printf '#pragma once\n#include "common/console_log.h"\n' > $@

build/include/airtime_gate.h:
	@mkdir -p build/include/cam
	@printf '#pragma once\n#include "dcc/airtime_gate.h"\n' > $@

$(TEST_BINS): $(PROJECT_HEADERS) | $(COMPAT_HEADERS)

build/tests/test_dcc_state_machine: src/dcc/dcc_state_machine.cpp
build/tests/test_dcc_manager: src/dcc/dcc_manager.cpp src/dcc/dcc_cross.cpp \
    src/dcc/dcc_access.cpp src/dcc/airtime_gate.cpp src/dcc/dcc_state_machine.cpp \
    src/radio/cbr_sampler.cpp src/net/dcc_net.cpp src/net/dcc_network.cpp \
    src/net/gn_receiver.cpp cam/cam_message.cpp cam/cam_sender.cpp $(C_OBJS)
build/tests/test_dcc_architecture: src/dcc/dcc_cross.cpp src/dcc/dcc_access.cpp \
    src/dcc/airtime_gate.cpp src/dcc/dcc_state_machine.cpp src/radio/cbr_sampler.cpp \
    src/net/dcc_net.cpp src/net/dcc_network.cpp src/net/gn_receiver.cpp
build/tests/test_dcc_control: src/dcc/airtime_gate.cpp
build/tests/test_cbr_sampler: src/radio/cbr_sampler.cpp
build/tests/test_cam: src/net/dcc_net.cpp cam/cam_message.cpp cam/cam_sender.cpp src/dcc/airtime_gate.cpp $(C_OBJS)
build/tests/test_dcc_net: src/net/dcc_net.cpp src/dcc/dcc_state_machine.cpp src/net/gn_receiver.cpp

build/tests/test_radio_netlink: tests/test_radio_netlink.cpp build/src/radio/CBRReader.o \
    build/src/radio/rate_manager.o build/src/common/utils.o
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(NL_CFLAGS) $(TEST_FLAGS) $(filter %.cpp %.o,$^) -o $@ -lssl -lcrypto -lm $(NL_LIBS)

build/tests/%: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(TEST_FLAGS) $(filter %.cpp %.o,$^) -o $@

test: test-cpp test-python

test-cpp: $(TEST_BINS)
	@set -e; for test_binary in $(TEST_BINS); do "./$$test_binary"; done

test-python:
	python3 -m unittest discover -s tests -p 'test_*.py' -v

clean:
	rm -rf build
	rm -f $(TARGET)
