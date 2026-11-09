#!/bin/sh
set -eu

rm -f /tmp/header_external_visibility.o /tmp/header_internal_visibility.o

if cc -Wall -Wextra -Werror -std=c11 -Iinclude -c test/header_external_visibility.c -o /tmp/header_external_visibility.o; then
    printf '%s\n' "unexpected success: external header still exposes debug query APIs"
    exit 1
fi

cc -Wall -Wextra -Werror -std=c11 -Iinclude -c test/header_internal_visibility.c -o /tmp/header_internal_visibility.o

# Desktop builds opt in explicitly; the default build stays closed.
cc -Wall -Wextra -Werror -std=c11 -DDUMP_PCM_DESKTOP_OBSERVE -Iinclude \
    -c test/header_desktop_observe.c -o /tmp/header_desktop_observe.o
