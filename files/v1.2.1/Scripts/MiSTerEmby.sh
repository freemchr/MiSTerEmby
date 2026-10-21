#!/bin/bash
# MiSTerEmby launcher

APP=/media/fat/misteremby/misteremby-arm

if [ -f /tmp/misteremby_apply_update.sh ]; then
    bash /tmp/misteremby_apply_update.sh
    rm -f /tmp/misteremby_apply_update.sh
fi

cd /media/fat/misteremby
"$APP"

# Main runs every script inside a wrapper that ends with "Press any key to
# continue" and waits for a key. Reloading the menu core through Main's own
# command interface goes straight back to the MiSTer menu instead.
# (Touch /media/fat/misteremby/keep-script-prompt to keep the prompt.)
if [ ! -e /media/fat/misteremby/keep-script-prompt ] && [ -w /dev/MiSTer_cmd ] && [ -f /media/fat/menu.rbf ]; then
    echo "load_core /media/fat/menu.rbf" > /dev/MiSTer_cmd
fi
