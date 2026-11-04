#!/bin/bash
# Host-side unit tests. No kernel headers or hardware needed.
set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-cc}
$CC -std=c11 -O2 -Wall -Wextra -Werror -o /tmp/test_assemble test_assemble.c
/tmp/test_assemble
$CC -std=c11 -O2 -Wall -Wextra -Werror -o /tmp/test_regplan test_regplan.c
/tmp/test_regplan
