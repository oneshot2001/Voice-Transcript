#!/usr/bin/env python3

import sys
import subprocess
import glob
import os

def main():
    if len(sys.argv) not in (4, 5):
        print(f"Usage: python {sys.argv[0]} <address> <user> <password> [aarch64|armv7hf]", file=sys.stderr)
        sys.exit(1)

    camera_host = sys.argv[1]
    username = sys.argv[2]
    password = sys.argv[3]
    arch = sys.argv[4] if len(sys.argv) == 5 else None

    if arch and arch not in ("aarch64", "armv7hf"):
        print("ERROR: Architecture must be 'aarch64' or 'armv7hf'", file=sys.stderr)
        sys.exit(1)

    pattern = f"*_{arch}.eap" if arch else "*.eap"
    eap_files = sorted(glob.glob(pattern), key=lambda path: os.path.getmtime(path), reverse=True)
    if not eap_files:
        print(f"ERROR: No .eap file found matching {pattern}", file=sys.stderr)
        sys.exit(1)
    if not arch and len(eap_files) > 1:
        print("ERROR: Multiple .eap files found; specify architecture: aarch64 or armv7hf", file=sys.stderr)
        for eap in eap_files:
            print(f"  {eap}", file=sys.stderr)
        sys.exit(1)

    eap_file = eap_files[0]
    print(f"Installing {eap_file} to {camera_host}...")

    # Upload using curl with digest authentication
    result = subprocess.run([
        "curl", "--digest",
        "-u", f"{username}:{password}",
        "-F", f"packfil=@{eap_file};type=application/octet-stream",
        f"http://{camera_host}/axis-cgi/applications/upload.cgi"
    ])

    if result.returncode != 0:
        print("ERROR: Upload failed", file=sys.stderr)
        sys.exit(result.returncode)

    print("\nDone.")

if __name__ == "__main__":
    main()
