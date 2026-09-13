#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run pvsched_kvm_hooks with the test-only KVM hook observer loaded.  A
# module that is already loaded is used and left alone; one loaded here is
# removed again.
# A built module that fails to load is a failure, not a skip.

dir=$(dirname "$0")
ko="$dir/test_modules/pvsched-hook-observer.ko"
loaded=0

if [ ! -e /dev/pvsched-hook-observer ] && [ -e "$ko" ]; then
	if ! out=$(insmod "$ko" 2>&1); then
		echo "# insmod $ko failed: $out"
		exit 1
	fi
	loaded=1
fi

"$dir/pvsched_kvm_hooks"
ret=$?

if [ "$loaded" -eq 1 ] && ! rmmod pvsched_hook_observer; then
	echo "# rmmod pvsched_hook_observer failed"
	[ "$ret" -eq 0 ] && ret=1
fi
exit $ret
