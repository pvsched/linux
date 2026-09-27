# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_PVSCHED) += pvsched.o
pvsched-y := core.o shm.o shm_bridge.o attachment.o lifecycle.o accounting.o runtime_accounting.o \
	    policy.o default_policy.o runner_runtime.o

obj-$(CONFIG_PVSCHED_ACCOUNTING_KUNIT_TEST) += pvsched-accounting-test.o
pvsched-accounting-test-y := accounting_test.o
obj-$(CONFIG_PVSCHED_RUNTIME_ACCOUNTING_KUNIT_TEST) += pvsched-runtime-accounting-test.o
pvsched-runtime-accounting-test-y := runtime_accounting_test.o
obj-$(CONFIG_PVSCHED_DEFAULT_POLICY_KUNIT_TEST) += pvsched-default-policy-test.o
pvsched-default-policy-test-y := default_policy_test.o

obj-$(CONFIG_PVSCHED_SCHEDULER_KUNIT_TEST) += pvsched-scheduler-test.o
pvsched-scheduler-test-y := scheduler_test.o

obj-$(CONFIG_PVSCHED_RUNNER_RUNTIME_KUNIT_TEST) += pvsched-runner-runtime-test.o
pvsched-runner-runtime-test-y := runner_runtime_test.o

obj-$(CONFIG_PVSCHED_ATTACHMENT_KUNIT_TEST) += pvsched-attachment-test.o
pvsched-attachment-test-y := attachment_test.o

obj-$(CONFIG_PVSCHED_LIFECYCLE_KUNIT_TEST) += pvsched-lifecycle-test.o
pvsched-lifecycle-test-y := lifecycle_test.o

obj-$(CONFIG_PVSCHED_SHM_KUNIT_TEST) += pvsched-shm-test.o
pvsched-shm-test-y := shm_test.o

obj-$(CONFIG_PVSCHED_POLICY_KUNIT_TEST) += pvsched-policy-test.o
pvsched-policy-test-y := policy_test.o
