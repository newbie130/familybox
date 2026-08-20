#!/usr/bin/env bash
# Writes your WiFi and relay settings into sdkconfig, which is gitignored.
# Nothing typed here is echoed to the screen or stored anywhere else.
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -f sdkconfig ]; then
  echo "No sdkconfig yet - run 'idf.py build' once first." >&2
  exit 1
fi

read -r -p "WiFi network name (2.4GHz): " SSID
read -r -s -p "WiFi password: " PASS; echo
read -r -p "Relay URL (e.g. http://192.168.1.50:8090): " URL
read -r -p "Relay token: " TOKEN

python3 - "$SSID" "$PASS" "$URL" "$TOKEN" <<'PY'
import sys, pathlib
ssid, password, url, token = sys.argv[1:5]
url = url.rstrip("/")
values = {
    "CONFIG_FB_WIFI_SSID": ssid,
    "CONFIG_FB_WIFI_PASSWORD": password,
    "CONFIG_FB_RELAY_URL": url,
    "CONFIG_FB_RELAY_TOKEN": token,
}
path = pathlib.Path("sdkconfig")
lines = path.read_text().splitlines()
out, seen = [], set()
for line in lines:
    key = line.split("=", 1)[0]
    if key in values:
        out.append(f'{key}="{values[key]}"')
        seen.add(key)
    else:
        out.append(line)
for key, val in values.items():
    if key not in seen:
        out.append(f'{key}="{val}"')
path.write_text("\n".join(out) + "\n")
print(f"  SSID  : {ssid}")
print(f"  Relay : {url}")
print( "  Password and token written (not shown).")
PY
echo
echo "Done. Now run:  idf.py build && idf.py -p <PORT> flash monitor"
echo "  (find <PORT> with: ls /dev/cu.* on macOS, or /dev/ttyACM* on Linux)"
