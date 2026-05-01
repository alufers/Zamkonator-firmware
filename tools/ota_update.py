#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["zeroconf>=0.131", "httpx>=0.27"]
# ///
"""OTA update tool for Zamkonator firmware.

Usage:
  ota_update.py firmware.bin               # discover via mDNS, prompt for password
  ota_update.py --host 192.168.1.50 fw.bin
  ota_update.py --host zamkonator.local --password secret fw.bin
  ota_update.py --no-password fw.bin
"""

import argparse
import getpass
import sys
import time
from pathlib import Path


def discover_device(timeout: float = 8.0) -> tuple[str, str, int] | None:
    """Return (name, ip, port) of the first discovered Zamkonator, or None."""
    from zeroconf import ServiceBrowser, ServiceStateChange, Zeroconf

    found: list[tuple[str, str, int]] = []

    def on_change(zeroconf: Zeroconf, service_type: str, name: str,
                  state_change: ServiceStateChange) -> None:
        if state_change != ServiceStateChange.Added:
            return
        info = zeroconf.get_service_info(service_type, name)
        if info and info.addresses:
            import socket
            ip = socket.inet_ntoa(info.addresses[0])
            found.append((name, ip, info.port))

    zc = Zeroconf()
    browser = ServiceBrowser(zc, "_zamkonator._tcp.local.", handlers=[on_change])

    print(f"Searching for Zamkonator devices (up to {timeout:.0f}s)…", flush=True)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not found:
        time.sleep(0.1)

    browser.cancel()
    zc.close()

    return found[0] if found else None


def progress_reader(path: Path, chunk_size: int = 65536):
    """Yield chunks from *path* and print upload progress to stderr."""
    total = path.stat().st_size
    sent = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            yield chunk
            sent += len(chunk)
            pct = sent / total * 100
            print(f"\r  {sent:,} / {total:,} bytes  ({pct:.1f}%)",
                  end="", flush=True, file=sys.stderr)
    print(file=sys.stderr)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="OTA update tool for Zamkonator firmware",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("firmware", type=Path, help="Firmware .bin file to upload")
    parser.add_argument("--host", metavar="HOST",
                        help="Device hostname or IP (skips mDNS discovery)")
    parser.add_argument("--port", type=int, default=80, metavar="PORT",
                        help="HTTP port (default: 80)")
    parser.add_argument("--password", "-p", metavar="PASSWORD",
                        help="Web UI password (will prompt if omitted)")
    parser.add_argument("--no-password", action="store_true",
                        help="Skip authentication entirely")
    args = parser.parse_args()

    firmware: Path = args.firmware
    if not firmware.exists():
        print(f"Error: file not found: {firmware}", file=sys.stderr)
        sys.exit(1)

    # ── Resolve host ──────────────────────────────────────────────────────────
    host = args.host
    port = args.port

    if not host:
        result = discover_device()
        if not result:
            print(
                "No Zamkonator device found via mDNS.\n"
                "Use --host to specify the device address.",
                file=sys.stderr,
            )
            sys.exit(1)
        name, ip, svc_port = result
        print(f"Found: {name}  →  {ip}:{svc_port}")
        try:
            confirm = input("Proceed with OTA update? [y/N] ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print("\nAborted.")
            sys.exit(0)
        if confirm != "y":
            print("Aborted.")
            sys.exit(0)
        host = ip
        port = svc_port

    # ── Password ──────────────────────────────────────────────────────────────
    password: str | None = None
    if not args.no_password:
        if args.password:
            password = args.password
        else:
            try:
                password = getpass.getpass(f"Password for {host}: ")
            except (EOFError, KeyboardInterrupt):
                print("\nAborted.")
                sys.exit(0)

    # ── Upload ────────────────────────────────────────────────────────────────
    base_url = f"http://{host}:{port}"
    headers: dict[str, str] = {"Content-Type": "application/octet-stream",
                                "Content-Length": str(firmware.stat().st_size)}
    if password:
        headers["X-Auth"] = password

    size_kb = firmware.stat().st_size / 1024
    print(f"Uploading {firmware.name} ({size_kb:.1f} KB) to {base_url} …")

    import httpx

    try:
        with httpx.Client(timeout=300.0) as client:
            resp = client.post(
                f"{base_url}/api/ota/upload",
                content=progress_reader(firmware),
                headers=headers,
            )
    except httpx.ConnectError as exc:
        print(f"Connection error: {exc}", file=sys.stderr)
        sys.exit(1)
    except httpx.ReadError:
        # Device may have reset mid-response after a successful write — treat as
        # success; the device handles its own reboot after writing the partition.
        print("Connection closed by device (likely rebooting after successful write).")
        sys.exit(0)

    if resp.status_code == 401:
        print("Authentication failed — check your password.", file=sys.stderr)
        sys.exit(1)

    if not resp.is_success:
        print(f"Upload failed: HTTP {resp.status_code}", file=sys.stderr)
        print(resp.text, file=sys.stderr)
        sys.exit(1)

    try:
        result = resp.json()
    except Exception:
        result = {}

    if result.get("ok"):
        print("Upload successful! Device is rebooting with new firmware.")
        print("The device will roll back if it does not obtain an IP within 20 s.")
    else:
        print(f"Unexpected response: {resp.text}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
