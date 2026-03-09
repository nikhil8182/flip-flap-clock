#\!/bin/bash
#
# Split-Flap Clock - Serial Monitor
#

BAUD=115200

find_port() {
    for port in /dev/cu.usbserial-* /dev/cu.wchusbserial-* /dev/cu.SLAB_USBtoUART* /dev/cu.usbmodem-*; do
        [ -e "$port" ] && echo "$port" && return 0
    done
    return 1
}

PORT=$(find_port)
if [ -z "$PORT" ]; then
    echo "No ESP32 found."
    exit 1
fi

pids=$(lsof -t "$PORT" 2>/dev/null)
[ -n "$pids" ] && echo "Freeing $PORT..." && echo "$pids" | xargs kill -9 2>/dev/null && sleep 1

echo "Port: $PORT"
echo "Baud: $BAUD"
echo "Exit: Ctrl+A then K, then Y"
echo ""
screen "$PORT" "$BAUD"
