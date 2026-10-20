#!/bin/bash
# MiSTerEmby launcher

APP=/media/fat/misteremby/misteremby-arm

if [ -f /tmp/misteremby_apply_update.sh ]; then
    bash /tmp/misteremby_apply_update.sh
    rm -f /tmp/misteremby_apply_update.sh
fi

cd /media/fat/misteremby
exec "$APP"
