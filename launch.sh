#! /bin/sh

if [ -e $SNAP_COMMON/babeld.pid ]; then
    if pgrep -F $SNAP_COMMON/babeld.pid; then
        echo "Babeld already running"
    else
        rm $SNAP_COMMON/babeld.pid
    fi
fi
exec babeld -c $SNAP_COMMON/babeld.conf
