#!/bin/sh
# Rebuild the self-contained set_zte_imei from src/ using Docker (aarch64 musl).
# Output: ./set_zte_imei  (helpers compiled, stripped, and embedded as base64).
set -eu
here=$(cd "$(dirname "$0")" && pwd); cd "$here"
docker run --rm --platform linux/arm64 -v "$PWD":/w -w /w alpine:3.20 sh -euc '
  apk add --no-cache gcc musl-dev coreutils >/dev/null
  mkdir -p .build
  gcc -O2 -s -Isrc -o .build/zte_config_b03  src/zte_config.c      -ldl -lpthread
  gcc -O2 -s -Isrc -o .build/zte_config_read src/zte_config_read.c -ldl -lpthread
  gcc -O2 -s       -o .build/diagnv          src/diagnv.c          -ldl
  gcc -O2 -s       -o .build/mkplan          src/mkplan.c
  cp src/set_zte_imei.template set_zte_imei
  for n in zte_config_b03 zte_config_read diagnv mkplan; do
    printf "#BLOB:%s\n" "$n" >> set_zte_imei
    base64 -w76 .build/$n    >> set_zte_imei
    printf "#ENDBLOB\n"      >> set_zte_imei
  done
  chmod 755 set_zte_imei
  rm -rf .build
  echo "built set_zte_imei ($(wc -c < set_zte_imei) bytes)"
'
