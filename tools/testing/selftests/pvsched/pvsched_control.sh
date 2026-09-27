#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run pvsched_control with the sample-nice host policy module loaded, so the
# test can select a policy other than the default.  A module that is already
# loaded is used and left alone; one loaded here is removed again, which
# also checks that closed sessions released it.  Without the built module,
# the sample-policy test is skipped.
# A built module that fails to load is a failure, not a skip.

dir=$(dirname "$0")
ko="$dir/test_modules/pvsched-sample-nice.ko"
loaded=0

# Without pvsched there is nothing to test, and the module cannot load.
if [ ! -e /dev/pvsched ] && [ ! -d /sys/module/pvsched ]; then
	echo "# SKIP: pvsched is not available"
	exit 4
fi

if [ ! -d /sys/module/pvsched_sample_nice ] && [ -e "$ko" ]; then
	if ! out=$(insmod "$ko" 2>&1); then
		echo "# insmod $ko failed: $out"
		exit 1
	fi
	loaded=1
fi

"$dir/pvsched_control"
ret=$?

if [ "$loaded" -eq 1 ] && ! rmmod pvsched_sample_nice; then
	echo "# rmmod pvsched_sample_nice failed"
	[ "$ret" -eq 0 ] && ret=1
fi
exit $ret
