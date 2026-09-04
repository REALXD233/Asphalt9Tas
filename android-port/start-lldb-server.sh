#!/system/bin/sh
rm -f /data/local/tmp/a9tas-lldb-server.log
/data/local/tmp/lldb-server gdbserver :50391 --attach "$1" \
  >/data/local/tmp/a9tas-lldb-server.log 2>&1 &
