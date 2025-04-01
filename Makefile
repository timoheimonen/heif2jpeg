# Makefile for heic_converter

# Compiler to use (clang++ is default on macOS with Xcode command line tools)
CXX = clang++

# --- Define Brew Prefix (Adjust if your brew installation is elsewhere) ---
BREW_PREFIX = /opt/homebrew

# Compiler flags:
# -std=c++17 : Enable C++17 standard (needed for std::filesystem)
# -I$(BREW_PREFIX)/include : Add brew's include path
# -Wall -Wextra : Enable most common warnings (good practice)
# -O2        : Optimization level (O0 or -g for debugging)
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I$(BREW_PREFIX)/include

# Linker flags:
# -L$(BREW_PREFIX)/lib : Add brew's library path
LDFLAGS = -L$(BREW_PREFIX)/lib

# Libraries to link against:
# -lheif : libheif library
# -ljpeg : libjpeg library (usually libjpeg-turbo via brew)
LIBS = -lheif -ljpeg

# Source file(s)
# Use the user's filename heic2jpeg.cpp
SRCS = heif2jpeg.cpp

# Target executable name
# Use the user's target name heic2jpeg
TARGET = heif2jpeg

# Default target: Build the executable
# This is the target that runs when you just type "make"
all: $(TARGET)

# Rule to build the target executable from the source file(s)
$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS) $(LIBS)

# Target to clean up generated files
clean:
	@echo "Cleaning up..."
	@rm -f $(TARGET) *.o  # Remove executable and any potential object files (-f ignores errors if files don't exist)

# Declare 'all' and 'clean' as phony targets, meaning they don't represent actual files
.PHONY: all clean