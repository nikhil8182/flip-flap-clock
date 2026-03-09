#!/bin/zsh
#
# Split-Flap Clock — Build & Flash
# Auto-detects ESP32 serial port, fixes IDF Python env, builds and flashes
#

IDF_PATH="$HOME/esp/esp-idf-v5.4.1"
IDF_VENV="$HOME/.espressif/python_env/idf5.4_py3.9_env"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BAUD=115200

# ── Auto-detect ESP32 serial port ──
find_port() {
    local patterns=(
        /dev/cu.usbserial-*
        /dev/cu.wchusbserial-*
        /dev/cu.SLAB_USBtoUART*
        /dev/cu.usbmodem-*
    )
    for pattern in "${patterns[@]}"; do
        for port in $pattern; do
            [ -e "$port" ] && echo "$port" && return 0
        done
    done
    return 1
}

# ── Free the port ──
free_port() {
    local port="$1"
    local pids
    pids=$(lsof -t "$port" 2>/dev/null)
    if [ -n "$pids" ]; then
        echo "Killing processes on $port: $pids"
        echo "$pids" | xargs kill -9 2>/dev/null
        sleep 1
    fi
}

# ── Fix IDF Python env (setuptools 82+ removes pkg_resources, ──
# ── ruamel.yaml.clib egg-info not discoverable by pkg_resources) ──
fix_idf_python() {
    local py="$IDF_VENV/bin/python"
    local site="$IDF_VENV/lib/python3.9/site-packages"

    # Fix 1: setuptools 82+ removed pkg_resources
    if ! "$py" -c "import pkg_resources" 2>/dev/null; then
        echo "Fixing setuptools..."
        "$py" -m pip install -q "setuptools<75" 2>&1
    fi

    # Fix 2: ruamel.yaml.clib egg-info missing (namespace package bug)
    if ! "$py" -c "import pkg_resources; pkg_resources.require('ruamel.yaml.clib')" 2>/dev/null; then
        echo "Fixing ruamel.yaml.clib discovery..."
        mkdir -p "$site/ruamel.yaml.clib-0.2.15.egg-info"
        cat > "$site/ruamel.yaml.clib-0.2.15.egg-info/PKG-INFO" << 'EOFPKG'
Metadata-Version: 1.0
Name: ruamel.yaml.clib
Version: 0.2.15
EOFPKG
        echo "_ruamel_yaml" > "$site/ruamel.yaml.clib-0.2.15.egg-info/top_level.txt"
    fi
}

# ── Main ──
echo "=== Split-Flap Clock v12 — Build & Flash ==="
echo ""

# Check IDF exists
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ERROR: ESP-IDF not found at $IDF_PATH"
    echo "Set IDF_PATH at the top of this script."
    exit 1
fi

# Fix Python env before sourcing
fix_idf_python

# Source IDF environment
echo "Sourcing ESP-IDF..."
source "$IDF_PATH/export.sh" 2>&1 | grep -v "^$"
if ! command -v idf.py &>/dev/null; then
    echo ""
    echo "ERROR: idf.py not found after sourcing export.sh"
    echo "Try running: cd $IDF_PATH && ./install.sh esp32"
    echo "Then: $IDF_VENV/bin/python -m pip install 'setuptools<75' 'ruamel.yaml<0.18'"
    exit 1
fi
echo ""

# Build
echo "Compiling..."
cd "$PROJECT_DIR"
idf.py build 2>&1
if [ $? -ne 0 ]; then
    echo ""
    echo "COMPILE FAILED"
    exit 1
fi

# Flash if port found (or build-only mode)
if [[ "$1" == "--build-only" || "$1" == "-b" ]]; then
    echo ""
    echo "=== Build Complete (flash skipped) ==="
    exit 0
fi

PORT=$(find_port)
if [ -z "$PORT" ]; then
    echo ""
    echo "No ESP32 found — build succeeded but skipping flash."
    echo "Searched: /dev/cu.usbserial-* /dev/cu.wchusbserial-* /dev/cu.SLAB_USBtoUART*"
    echo "Connect ESP32 and re-run, or flash manually: idf.py flash -p /dev/cu.xxx"
    exit 0
fi
echo ""
echo "Port: $PORT"

free_port "$PORT"

echo "Flashing to $PORT..."
idf.py flash -p "$PORT" 2>&1
if [ $? -ne 0 ]; then
    echo ""
    echo "FLASH FAILED — try holding BOOT button on ESP32 during flash"
    exit 1
fi

echo ""
echo "=== Flash Complete ==="

# Auto-open serial monitor if --monitor or -m flag
if [[ "$1" == "--monitor" || "$1" == "-m" ]]; then
    echo ""
    echo "Opening serial monitor ($BAUD baud)..."
    sleep 2
    idf.py monitor -p "$PORT"
fi
