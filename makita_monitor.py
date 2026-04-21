"""
Makita Monitor v1.0
Battery Diagnostic Monitor for Makita LXT batteries.
"""

import serial
import serial.tools.list_ports
import sys
import time
import os
import threading

BAUDRATE     = 115200
CONNECT_WAIT = 1.0
VERSION      = "1.0.0"
DEVICE_HINTS = ["makita monitor", "rp2040", "pico", "waveshare", "usb serial device"]

RESET  = "\033[0m"
GREEN  = "\033[92m"
RED    = "\033[91m"
YELLOW = "\033[93m"
CYAN   = "\033[96m"
BOLD   = "\033[1m"


def set_title(title: str):
    if sys.platform == "win32":
        os.system(f"title {title}")


def colourise(line: str) -> str:
    if "UNLOCKED" in line:
        return f"{GREEN}{BOLD}{line}{RESET}"
    if "LOCKED" in line:
        return f"{RED}{BOLD}{line}{RESET}"
    if "ERROR" in line or "WARN" in line:
        return f"{YELLOW}{line}{RESET}"
    if line.startswith("=") or line.startswith("-"):
        return f"{CYAN}{line}{RESET}"
    if "Health" in line:
        return f"{GREEN}{line}{RESET}"
    if "Complete." in line:
        return f"{CYAN}{BOLD}{line}{RESET}"
    return line


def wait_for_device() -> str:
    print(f"{CYAN}Searching for Makita Monitor device...{RESET}")
    while True:
        for port in serial.tools.list_ports.comports():
            if any(h in (port.description or "").lower() for h in DEVICE_HINTS):
                return port.device
        time.sleep(1.0)


def reader_thread(ser: serial.Serial, stop: threading.Event):
    while not stop.is_set():
        try:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            if line:
                print(colourise(line))
        except serial.SerialException:
            break
        except Exception:
            continue


def print_header(port: str):
    print(f"{CYAN}{'=' * 44}{RESET}")
    print(f"{CYAN}{BOLD}  Makita Monitor  v{VERSION}{RESET}")
    print(f"{CYAN}  Connected  :  {port}{RESET}")
    print(f"{CYAN}  Enter = rescan   Ctrl+C = quit{RESET}")
    print(f"{CYAN}{'=' * 44}{RESET}\n")


def main():
    set_title("Makita Monitor")
    print(f"\n{CYAN}{BOLD}  Makita Monitor  v{VERSION}{RESET}\n")

    port = wait_for_device()
    print(f"{GREEN}  Device found on {port}{RESET}\n")

    try:
        ser = serial.Serial(port, BAUDRATE, timeout=1)
    except serial.SerialException as e:
        print(f"{RED}ERROR: {e}{RESET}")
        input("Press Enter to exit...")
        sys.exit(1)

    time.sleep(CONNECT_WAIT)
    ser.reset_input_buffer()
    print_header(port)

    stop = threading.Event()
    t    = threading.Thread(target=reader_thread, args=(ser, stop), daemon=True)
    t.start()

    ser.write(b's')  # auto-scan on connect

    try:
        while True:
            input()
            print(f"{CYAN}  Triggering new scan...{RESET}")
            ser.write(b's')
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        ser.close()
        print(f"\n{CYAN}  Disconnected. Goodbye.{RESET}\n")


if __name__ == "__main__":
    main()