# SPDX-License-Identifier: GPL-2.0

CFLAGS += -Wall -Wextra -O2 -pthread $(KHDR_INCLUDES)
TEST_GEN_MODS_DIR := test_modules
TEST_GEN_PROGS := pvsched_abi pvsched_control
TEST_GEN_PROGS_EXTENDED := pvsched_kvm_hooks
TEST_PROGS := pvsched_kvm_hooks.sh

include ../lib.mk
