// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/module.h>
#include <linux/string.h>

#include "default_policy.h"
#include "policy.h"

static const struct pvsched_policy_ops pvsched_policy_test_template = {
	.name = "kunit-policy",
	.version = 3,
	.protocol = PVSCHED_PROTOCOL_DEFAULT,
	.params_size = 40,
};

static void pvsched_policy_test_unregister(void *ops)
{
	pvsched_unregister_policy(ops);
}

/*
 * Test policies are static and unregistered by a cleanup action, so that a
 * failed assertion never leaves a registered policy behind.
 */
static void pvsched_policy_test_prepare(struct kunit *test,
					struct pvsched_policy_ops *ops)
{
	*ops = pvsched_policy_test_template;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test,
			pvsched_policy_test_unregister, ops), 0);
}

static void pvsched_policy_default_listed_test(struct kunit *test)
{
	struct pvsched_policy_entry *entry;

	entry = pvsched_policy_lookup(PVSCHED_DEFAULT_POLICY_NAME,
				      PVSCHED_DEFAULT_POLICY_VERSION);
	KUNIT_ASSERT_NOT_NULL(test, entry);
	KUNIT_EXPECT_PTR_EQ(test, entry->ops, &pvsched_default_policy_ops);
	KUNIT_EXPECT_PTR_EQ(test, entry->owner, entry->ops->owner);
	KUNIT_EXPECT_FALSE(test, entry->dead);
	KUNIT_EXPECT_EQ(test, entry->ops->params_size,
			(u32)sizeof(struct pvsched_default_params));
	pvsched_policy_entry_put(entry);
	KUNIT_EXPECT_NULL(test, pvsched_policy_lookup(PVSCHED_DEFAULT_POLICY_NAME,
						      PVSCHED_DEFAULT_POLICY_VERSION + 1));
	KUNIT_EXPECT_NULL(test, pvsched_policy_lookup("defaul", 1));
}

static void pvsched_policy_register_refusals_test(struct kunit *test)
{
	static struct pvsched_policy_ops ops;

	/* Each invalid field alone refuses the registration. */
	pvsched_policy_test_prepare(test, &ops);
	memset(ops.name, 0, sizeof(ops.name));
	KUNIT_EXPECT_EQ(test, pvsched_register_policy(&ops), -EINVAL);
	ops = pvsched_policy_test_template;
	memset(ops.name, 'x', sizeof(ops.name));
	KUNIT_EXPECT_EQ(test, pvsched_register_policy(&ops), -EINVAL);
	ops = pvsched_policy_test_template;
	ops.name[sizeof(ops.name) - 1] = 'x';
	KUNIT_EXPECT_EQ(test, pvsched_register_policy(&ops), -EINVAL);
	ops = pvsched_policy_test_template;
	ops.params_size = 0;
	KUNIT_EXPECT_EQ(test, pvsched_register_policy(&ops), -EINVAL);
	ops = pvsched_policy_test_template;
	ops.params_size = PVSCHED_POLICY_PARAMS_MAX + 1;
	KUNIT_EXPECT_EQ(test, pvsched_register_policy(&ops), -EINVAL);
	ops = pvsched_policy_test_template;
	ops.protocol = PVSCHED_PROTOCOL_CUSTOM;
	KUNIT_EXPECT_EQ(test, pvsched_register_policy(&ops), -EINVAL);
	KUNIT_EXPECT_NULL(test, pvsched_policy_lookup("kunit-policy", 3));

	/* The largest size is accepted. */
	ops = pvsched_policy_test_template;
	ops.params_size = PVSCHED_POLICY_PARAMS_MAX;
	KUNIT_ASSERT_EQ(test, pvsched_register_policy(&ops), 0);
	pvsched_unregister_policy(&ops);
}

static void pvsched_policy_duplicate_test(struct kunit *test)
{
	static struct pvsched_policy_ops first, second;
	struct pvsched_policy_entry *entry;

	pvsched_policy_test_prepare(test, &first);
	pvsched_policy_test_prepare(test, &second);

	KUNIT_ASSERT_EQ(test, pvsched_register_policy(&first), 0);
	/* The register macro records the calling module as the owner. */
	KUNIT_EXPECT_PTR_EQ(test, first.owner, THIS_MODULE);
	KUNIT_EXPECT_EQ(test, pvsched_register_policy(&second), -EEXIST);
	KUNIT_EXPECT_EQ(test, pvsched_register_policy(&first), -EEXIST);
	entry = pvsched_policy_lookup("kunit-policy", 3);
	KUNIT_ASSERT_NOT_NULL(test, entry);
	KUNIT_EXPECT_PTR_EQ(test, entry->ops, &first);
	KUNIT_EXPECT_PTR_EQ(test, entry->owner, THIS_MODULE);
	pvsched_policy_entry_put(entry);

	/* Another version of the same name is a separate policy. */
	second.version = 4;
	KUNIT_ASSERT_EQ(test, pvsched_register_policy(&second), 0);
	entry = pvsched_policy_lookup("kunit-policy", 4);
	KUNIT_ASSERT_NOT_NULL(test, entry);
	KUNIT_EXPECT_PTR_EQ(test, entry->ops, &second);
	pvsched_policy_entry_put(entry);
	pvsched_unregister_policy(&second);
	pvsched_unregister_policy(&first);
}

static void pvsched_policy_unregister_unlists_test(struct kunit *test)
{
	static struct pvsched_policy_ops ops;
	struct pvsched_policy_entry *entry;

	pvsched_policy_test_prepare(test, &ops);
	KUNIT_ASSERT_EQ(test, pvsched_register_policy(&ops), 0);
	entry = pvsched_policy_lookup("kunit-policy", 3);
	KUNIT_ASSERT_NOT_NULL(test, entry);
	pvsched_unregister_policy(&ops);
	/* Unlisted at once, while the reference keeps the entry alive. */
	KUNIT_EXPECT_NULL(test, pvsched_policy_lookup("kunit-policy", 3));
	KUNIT_EXPECT_TRUE(test, entry->dead);
	KUNIT_EXPECT_PTR_EQ(test, entry->ops, &ops);
	/* The name is free again. */
	KUNIT_EXPECT_EQ(test, pvsched_register_policy(&ops), 0);
	pvsched_unregister_policy(&ops);
	pvsched_policy_entry_put(entry);
	/* Unregistering an unlisted policy does nothing. */
	pvsched_unregister_policy(&ops);
}

static struct kunit_case pvsched_policy_test_cases[] = {
	KUNIT_CASE(pvsched_policy_default_listed_test),
	KUNIT_CASE(pvsched_policy_register_refusals_test),
	KUNIT_CASE(pvsched_policy_duplicate_test),
	KUNIT_CASE(pvsched_policy_unregister_unlists_test),
	{}
};

static struct kunit_suite pvsched_policy_test_suite = {
	.name = "pvsched-policy",
	.test_cases = pvsched_policy_test_cases,
};

kunit_test_suite(pvsched_policy_test_suite);

MODULE_DESCRIPTION("KUnit tests for the pvsched host policy registry");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
