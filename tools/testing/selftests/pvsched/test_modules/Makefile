# SPDX-License-Identifier: GPL-2.0
TESTMODS_DIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m += pvsched-hook-observer.o
pvsched-hook-observer-y := hook_observer.o

# Ensure that KDIR exists, otherwise skip the compilation
modules:
ifneq ("$(wildcard $(KDIR))", "")
	$(Q)$(MAKE) -C $(KDIR) modules KBUILD_EXTMOD=$(TESTMODS_DIR)
endif

# Ensure that KDIR exists, otherwise skip the clean target
clean:
ifneq ("$(wildcard $(KDIR))", "")
	$(Q)$(MAKE) -C $(KDIR) clean KBUILD_EXTMOD=$(TESTMODS_DIR)
endif
