# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_PVSCHED) += pvsched.o
pvsched-y := core.o accounting.o default_policy.o

obj-$(CONFIG_PVSCHED_ACCOUNTING_KUNIT_TEST) += pvsched-accounting-test.o
pvsched-accounting-test-y := accounting_test.o
