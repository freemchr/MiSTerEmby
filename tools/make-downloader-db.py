#!/usr/bin/env python3
"""Builds the MiSTer Downloader database for a MiSTerEmby release.

MiSTer Downloader (and update_all) can install and update MiSTerEmby
alongside everything else once downloader_misteremby.ini is on the SD card.
That file points at db.json on this repo's `db` branch, which this script
writes. The release workflow runs it on every tagged release.

Usage: make-downloader-db.py <release-dir> <version> <out-dir>

  release-dir  the packaged release tree (holds misteremby/)
  version      the release tag, e.g. v1.1.0
  out-dir      a checkout of the db branch. The release's files are copied
               to files/<version>/ in it, and db.json is rewritten to point
               at them. Earlier versions' files are left alone, so a
               Downloader that hasn't updated yet can still fetch them.

Standard library only.
"""

import hashlib
import json
import os
import shutil
import sys
import time

DB_ID = "freemchr/misteremby"
REPO_RAW = "https://raw.githubusercontent.com/freemchr/MiSTerEmby/db/"
APP_DIR = "misteremby"
LAUNCHER = APP_DIR + "/MiSTerEmby.sh"
# The user's own settings and sign-in. A database entry for either would
# overwrite them on every update.
USER_FILES = {APP_DIR + "/emby.conf", APP_DIR + "/token.conf", APP_DIR + "/device.conf"}


def destination(rel):
    # The menu only lists scripts in Scripts/, so the launcher goes there;
    # everything else installs into /media/fat/misteremby as the zip lays it out.
    return "Scripts/MiSTerEmby.sh" if rel == LAUNCHER else rel


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    release_dir, version, out_dir = sys.argv[1:]

    sources = []
    for root, _, names in os.walk(os.path.join(release_dir, APP_DIR)):
        for name in names:
            full = os.path.join(root, name)
            sources.append((os.path.relpath(full, release_dir).replace(os.sep, "/"), full))
    sources.sort()

    shipped = {rel for rel, _ in sources}
    if shipped & USER_FILES:
        sys.exit("release ships a user file: " + ", ".join(sorted(shipped & USER_FILES)))
    for needed in (LAUNCHER, APP_DIR + "/misteremby-arm", APP_DIR + "/mplayer-arm"):
        if needed not in shipped:
            sys.exit("release is missing " + needed)

    files_root = os.path.join(out_dir, "files", version)
    files, folders = {}, {}
    for rel, full in sources:
        dest = destination(rel)
        target = os.path.join(files_root, dest)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copy2(full, target)
        with open(full, "rb") as f:
            data = f.read()
        files[dest] = {"hash": hashlib.md5(data).hexdigest(), "size": len(data),
                       "tags": ["misteremby"]}
        parent = os.path.dirname(dest)
        while parent:
            folders[parent + "/"] = {"tags": ["misteremby"]}
            parent = os.path.dirname(parent)

    db = {
        "v": 1,
        "db_id": DB_ID,
        "timestamp": int(time.time()),
        "base_files_url": REPO_RAW + "files/" + version + "/",
        "files": dict(sorted(files.items())),
        "folders": dict(sorted(folders.items())),
    }
    with open(os.path.join(out_dir, "db.json"), "w") as f:
        json.dump(db, f, indent=1)
        f.write("\n")
    print("db.json: %d files, %d folders, %s" % (len(files), len(folders), version))


if __name__ == "__main__":
    main()
