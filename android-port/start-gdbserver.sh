#!/system/bin/sh
rm -f /data/local/tmp/a9tas-gdbserver.log
/system/bin/gdbserver --attach :50391 "$1" \
  >/data/local/tmp/a9tas-gdbserver.log 2>&1 &
