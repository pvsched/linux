# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_PVSCHED) += pvsched.o
pvsched-y := core.o accounting.o default_policy.o

obj-$(CONFIG_PVSCHED_ACCOUNTING_KUNIT_TEST) += pvsched-accounting-test.o
pvsched-accounting-test-y := accounting_test.o
obj-$(CONFIG_PVSCHED_DEFAULT_POLICY_KUNIT_TEST) += pvsched-default-policy-test.o
pvsched-default-policy-test-y := default_policy_test.o

obj-$(CONFIG_PVSCHED_SCHEDULER_KUNIT_TEST) += pvsched-scheduler-test.o
pvsched-scheduler-test-y := scheduler_test.o
