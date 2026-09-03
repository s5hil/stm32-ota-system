#!/usr/bin/env python3

import json
import os
import sys
import requests

from stm32_crc import stm32_crc32

REPO = "s5hil/stm32-ota-system"
API_URL = f"https://api.github.com/repos/{REPO}/releases/latest"

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.join(BASE_DIR, "firmware")
METADATA_FILE = os.path.join(BASE_DIR, "metadata.json")

SLOT_ASSETS = {
    "slot_a": "app_slot_a.bin",
    "slot_b": "app_slot_b.bin",
}

def current_build():
    # build number in metadata.json, or 0 if none
    if not os.path.exists(METADATA_FILE):
        return 0
    try:
        with open(METADATA_FILE) as f:
            return int(json.load(f).get("build", 0))
    except (ValueError, json.JSONDecodeError):
        print("metadata.json unreadable, reading as build 0")
        return 0

def parse_build(tag):
    # turn a tag like 'v6' into the integer 6. returns None if malformed
    if not tag.startswith("v"):
        return None
    try:
        return int(tag[1:])
    except ValueError:
        return None

def fetch_latest_release():
    res = requests.get(API_URL, timeout=15)
    if res.status_code != 200:
        print(f"Github API returned {res.status_code}")
        return None
    return res.json()

def download(url, dest):
    res = requests.get(url, timeout=60)
    if res.status_code != 200:
        print(f"Failed to download from {url}: HTTP {res.status_code}")
        return False
    with open(dest, "wb") as f:
        f.write(res.content)
    return True

def main():
    release = fetch_latest_release()
    if release is None:
        return 1

    tag = release.get("tag_name", "")
    build = parse_build(tag)

    if build is None:
        print(f"Tag '{tag}' is not in the expected vN form, ignoring")
        return 1

    installed = current_build()
    if build <= installed:
        print(f"Release {tag} is not newer than build {installed}, nothing to do")
        return 0

    print(f"New release {tag} (build {build}), current is {installed}")

    # map asset filename -> download URL
    assets = {a["name"]: a["browser_download_url"] for a in release.get("assets", [])}

    for filename in SLOT_ASSETS.values():
        if filename not in assets:
            print(f"Release {tag} is missing asset {filename}, aborting")
            return 1

    os.makedirs(FIRMWARE_DIR, exist_ok=True)

    # download to temp names so a partial failure leaves the previous
    # firmware and metadata untouched

    staged = {}

    for slot, filename in SLOT_ASSETS.items():
        tmp_path = os.path.join(FIRMWARE_DIR, filename + ".tmp")

        if not download(assets[filename], tmp_path):
            return 1

        with open(tmp_path, "rb") as f:
            data = f.read()

        if len(data) % 4 != 0:
            print(f"{filename} is {len(data)} bytes, not a multiple of 4 - aborting")
            return 1

        staged[slot] = {
            "tmp_path": tmp_path,
            "final_path": os.path.join(FIRMWARE_DIR, filename),
            "filename": filename,
            "size": len(data),
            "crc32": f"0x{stm32_crc32(data):08X}"
        }

        print(f"  {filename}: {len(data)} bytes, crc {staged[slot]['crc32']}")

    # everything downloaded and checked -> commit
    for slot, info in staged.items():
        os.replace(info["tmp_path"], info["final_path"])

    metadata = {
        "version": tag,
        "build": build,
        "slot_a": {
            "filename": staged["slot_a"]["filename"],
            "crc32": staged["slot_a"]["crc32"],
            "size": staged["slot_a"]["size"]
        },
        "slot_b": {
            "filename": staged["slot_b"]["filename"],
            "crc32": staged["slot_b"]["crc32"],
            "size": staged["slot_b"]["size"]
        }
    }

    with open(METADATA_FILE, "w") as f:
        json.dump(metadata, f, indent=2)

    print(f"metadata.json updated to build {build}")
    return 0

if __name__ == "__main__":
    sys.exit(main())