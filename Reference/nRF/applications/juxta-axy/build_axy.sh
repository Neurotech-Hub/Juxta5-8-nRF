#!/bin/bash

# Build script for JUXTA-AXY application
# Accelerometer playground for Juxta5-1_AXY boards

set -e

# Configuration
BOARD="Juxta5-1_AXY"
APPLICATION="juxta-axy"
BUILD_DIR="build"

echo "=========================================="
echo "Building JUXTA-AXY Application"
echo "=========================================="
echo "Board: $BOARD"
echo "Application: $APPLICATION"
echo "Build Directory: $BUILD_DIR"
echo "=========================================="

# Check if west is available
if ! command -v west &> /dev/null; then
    echo "❌ West command not found!"
    echo ""
    echo "This script requires the nRF Connect SDK environment to be active."
    echo ""
    echo "Alternative build methods:"
    echo "1. Use nRF Connect for VS Code extension:"
    echo "   - Open this directory in VS Code"
    echo "   - Use the nRF extension to build and flash"
    echo ""
    echo "2. Activate nRF Connect SDK environment first:"
    echo "   - Source the environment setup script"
    echo "   - Then run this script again"
    echo ""
    echo "3. Manual build command:"
    echo "   west build -b $BOARD applications/$APPLICATION"
    echo ""
    exit 1
fi

# Clean previous build
if [ -d "$BUILD_DIR" ]; then
    echo "Cleaning previous build..."
    rm -rf "$BUILD_DIR"
fi

# Build the application
echo "Building application..."
west build -b "$BOARD" -d "$BUILD_DIR" .

if [ $? -eq 0 ]; then
    echo "=========================================="
    echo "✅ Build successful!"
    echo "=========================================="
    echo "Binary location: $BUILD_DIR/zephyr/zephyr.hex"
    echo "ELF location: $BUILD_DIR/zephyr/zephyr.elf"
    echo ""
    echo "To flash the application:"
    echo "  west flash -d $BUILD_DIR"
    echo ""
    echo "To view logs:"
    echo "  west monitor -d $BUILD_DIR"
    echo "=========================================="
else
    echo "=========================================="
    echo "❌ Build failed!"
    echo "=========================================="
    exit 1
fi 