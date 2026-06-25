# Compiler settings
CC = gcc
CXX = g++

# Netlink flags necessari per CBRReader
NL_CFLAGS := $(shell pkg-config --cflags libnl-3.0 libnl-genl-3.0 2>/dev/null || echo -I/usr/include/libnl3)
NL_LIBS   := $(shell pkg-config --libs libnl-3.0 libnl-genl-3.0 2>/dev/null || echo -lnl-3 -lnl-genl-3)

# -Idcc permette di trovare tutti gli header nella cartella dcc
CFLAGS = -c -Wall -IUtilsEncode
CXXFLAGS = -c -Wall -IUtilsEncode $(NL_CFLAGS) -std=c++17
LDFLAGS = -lpthread -lssl -lcrypto -lm $(NL_LIBS)

# Directories
C_DIR = UtilsEncode
CXX_DIR = .
OUTPUT_DIR = .

# C source files
C_SOURCES := $(wildcard $(C_DIR)/*.c)

# C++ source files (AGGIUNTO dcc/utils.cpp alla lista)
CXX_SOURCES := main.cpp CBRReader.cpp utils.cpp TxPowerManager.cpp dcc_manager.cpp rate_manager.cpp

# Object files
C_OBJS := $(patsubst $(C_DIR)/%.c, $(C_DIR)/%.o, $(C_SOURCES))
CXX_OBJS := $(patsubst %.cpp, %.o, $(CXX_SOURCES))

# Output binary in the main folder
TARGET = dcc

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
	rm -f $(C_OBJS) $(CXX_OBJS) $(TARGET)