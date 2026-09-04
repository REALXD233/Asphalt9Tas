#!/system/bin/sh
rm -f /data/local/tmp/a9tas-inject.log
/data/local/tmp/a9tas_injector --wait com.aligames.kuang.kybc.aligames \
  /data/local/tmp/liba9tas_bootstrap.so \
  >/data/local/tmp/a9tas-inject.log 2>&1 &
