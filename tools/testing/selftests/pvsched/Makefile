# SPDX-License-Identifier: GPL-2.0

CFLAGS += -Wall -Wextra -O2 -pthread -I$(top_srcdir)/tools/testing/selftests $(KHDR_INCLUDES)
TEST_GEN_MODS_DIR := test_modules
TEST_GEN_PROGS := pvsched_abi
TEST_GEN_PROGS_EXTENDED := pvsched_control pvsched_kvm_hooks
TEST_PROGS := pvsched_control.sh pvsched_kvm_hooks.sh

include ../lib.mk
