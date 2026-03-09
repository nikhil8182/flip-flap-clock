#\!/bin/zsh
#
# Split-Flap Clock - Dev Workflow
# Usage: ./dev.sh          Build + Flash + Monitor
#        ./dev.sh build    Build only
#        ./dev.sh mon      Monitor only
#

IDF_PATH="$HOME/esp/esp-idf-v5.4.1"
BAUD=115200

# Auto-detect ESP32 serial port
find_port() {
    for port in /dev/cu.usbserial-* /dev/cu.wchusbserial-* /dev/cu.SLAB_USBtoUART* /dev/cu.usbmodem-*; do
        [ -e "$port" ] && echo "$port" && return 0
    done
    return 1
}

# Kill anything on the port
free_port() {
    local pids=$(lsof -t "$1" 2>/dev/null)
    [ -n "$pids" ] && echo "Freeing $1..." && echo "$pids" | xargs kill -9 2>/dev/null && sleep 1
}

# Source IDF
setup_idf() {
    if \! command -v idf.py &>/dev/null; then
        if [ \! -f "$IDF_PATH/export.sh" ]; then
            echo "ERROR: ESP-IDF not found at $IDF_PATH"
            exit 1
        fi
        source "$IDF_PATH/export.sh" 2>/dev/null
    fi
}

MODE="${1:-all}"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

case "$MODE" in
    mon|monitor)
        PORT=$(find_port)
        [ -z "$PORT" ] && echo "No ESP32 found." && exit 1
        free_port "$PORT"
        echo "Monitor: $PORT @ $BAUD"
        echo "Exit: Ctrl+A then K, then Y"
        screen "$PORT" "$BAUD"
        ;;
    build|b)
        setup_idf
        cd "$PROJECT_DIR"
        echo "Building..."
        idf.py build 2>&1 | tail -3
        ;;
    *)
        setup_idf
        cd "$PROJECT_DIR"
        echo "=== Building ==="
        idf.py build 2>&1 | tail -3
        [ $? -ne 0 ] && echo "BUILD FAILED" && exit 1

        PORT=$(find_port)
        if [ -z "$PORT" ]; then
            echo "Build OK but no ESP32 found."
            exit 0
        fi

        free_port "$PORT"
        echo "\n=== Flashing $PORT ==="
        idf.py flash -p "$PORT" 2>&1 | tail -5
        [ $? -ne 0 ] && echo "FLASH FAILED" && exit 1

        echo "\n=== Monitor (Ctrl+A K to exit) ==="
        sleep 1
        screen "$PORT" "$BAUD"
        ;;
esac
