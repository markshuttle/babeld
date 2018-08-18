#! /bin/sh

if [ -e $SNAP_COMMON/babeld.pid ]; then
    if pgrep -F $SNAP_COMMON/babeld.pid; then
        echo "Babeld already running"
        exit 1
    else
        rm $SNAP_COMMON/babeld.pid
    fi
fi

if [ ! -e $SNAP_COMMON/babeld.conf ]; then
    /bin/cat <<EOM >$SNAP_COMMON/babeld.conf
# For more information about this configuration file, refer to
# https://www.irif.fr/~jch/software/babel/babeld.html
pid-file /var/snap/babeld-sabdfl/common/babeld.pid
log-file /var/snap/babeld-sabdfl/common/babeld.log
state-file /var/snap/babeld-sabdfl/common/babeld.state
link-detect true
reflect-kernel-metric true
interface eth0 type wired rxcost 100
redistribute local deny
EOM
fi
exec $SNAP/babeld -c $SNAP_COMMON/babeld.conf -S $SNAP_COMMON/babeld.state
