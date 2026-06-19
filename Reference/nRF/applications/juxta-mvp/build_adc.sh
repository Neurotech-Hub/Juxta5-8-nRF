#!/bin/bash

# Build script for Juxta5-1_ADC board
# This script builds the juxta-mvp application for the ADC board variant

echo "Building juxta-mvp for Juxta5-1_ADC board..."

# Clean previous build
rm -rf build

# Build the application
west build -b Juxta5-1_ADC applications/juxta-mvp

if [ $? -eq 0 ]; then
    echo "✅ Build successful!"
    echo ""
    echo "To flash the application:"
    echo "  west flash"
    echo ""
    echo "To debug the application:"
    echo "  west debug"
    echo ""
    echo "Note: This builds the original example application."
    echo "To test board-specific ADC/FRAM/GPIO functionality, edit main.c and uncomment:"
    echo "  #define USE_BOARD_SPECIFIC_EXAMPLE"
else
    echo "❌ Build failed!"
    exit 1
fi 