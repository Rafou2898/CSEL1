# Makefile for CMake project with intelligent configuration

TOOLCHAIN_PATH=/buildroot/output/host/usr/bin/
CROSS_CC=$(TOOLCHAIN_PATH)aarch64-linux-gcc

LD=$(TOOLCHAIN)gcc
AR=$(TOOLCHAIN)ar

# Default target
all: build/build.ninja 
	cmake --build build

# Create build directory and generate build files if needed
build/build.ninja : CMakeLists.txt
	cmake -S . -B build -G "Ninja" -DCMAKE_C_COMPILER=$(CROSS_CC)

# Clean build directory
clean:
	rm -rf build

# Rebuild from scratch
rebuild: clean all

# Phony targets (targets that don't represent files)
.PHONY: all clean rebuild

