// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/pvsched.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <unistd.h>

#include "kselftest.h"

static void test_exec_fd(void);

/* The limits the kernel advertises; the quota tests fill them. */
static struct pvsched_info limits;

static int check_info(int fd)
{
	struct pvsched_info info;

	memset(&info, 0xa5, sizeof(info));
	if (ioctl(fd, PVSCHED_GET_INFO, &info) < 0)
		return -errno;
	if (info.control_version != PVSCHED_CONTROL_VERSION || info.flags ||
	    !info.max_runners_per_session || !info.max_runners_global ||
	    !info.max_sessions_global || !info.max_shm_pages_per_session ||
	    !info.max_shm_pages_global || info.reserved)
		return -EINVAL;
	limits = info;
	return 0;
}

static int set_policy(int fd, const char *name, uint32_t version)
{
	struct pvsched_set_policy set = { .version = version };

	strncpy(set.name, name, sizeof(set.name) - 1);
	return ioctl(fd, PVSCHED_SET_POLICY, &set) < 0 ? -errno : 0;
}

/* Open a session and select the default policy, as a VMM must first. */
static int open_session(void)
{
	int fd = open("/dev/pvsched", O_RDWR | O_CLOEXEC);
	int rc;

	if (fd < 0)
		return fd;
	rc = set_policy(fd, PVSCHED_DEFAULT_POLICY_NAME,
			PVSCHED_DEFAULT_POLICY_VERSION);
	if (rc) {
		close(fd);
		errno = -rc;
		return -1;
	}
	return fd;
}

static int create_self(int fd, struct pvsched_create_runner *request)
{
	memset(request, 0, sizeof(*request));
	request->tid = (int)gettid();
	return ioctl(fd, PVSCHED_CREATE_RUNNER, request) < 0 ? -errno : 0;
}

static int create_tid(int fd, pid_t tid, uint64_t *runner_id)
{
	struct pvsched_create_runner request = { .tid = tid };
	int rc;

	rc = ioctl(fd, PVSCHED_CREATE_RUNNER, &request) < 0 ? -errno : 0;
	if (!rc && runner_id)
		*runner_id = request.runner_id;
	return rc;
}

static int query(int fd, uint64_t id, struct pvsched_query_runner *query)
{
	memset(query, 0, sizeof(*query));
	query->runner_id = id;
	return ioctl(fd, PVSCHED_QUERY_RUNNER, query) < 0 ? -errno : 0;
}

/*
 * Wait up to about two seconds for a runner's exit to be reported as
 * EXITED.  A runner that was never attached must be INACTIVE until then.
 */
static int wait_exited(int fd, uint64_t id, bool attached)
{
	int i;

	for (i = 0; i < 2000; i++) {
		struct pvsched_query_runner result;
		int rc = query(fd, id, &result);

		if (rc)
			return rc;
		if (result.state == PVSCHED_RUNNER_EXITED)
			return 0;
		if (!attached && result.state != PVSCHED_RUNNER_INACTIVE)
			return -EINVAL;
		usleep(1000);
	}
	return -ETIMEDOUT;
}

/* Run @child_fn in a forked child; true if it exits with status 0. */
static bool fork_expect(int (*child_fn)(int fd), int fd, const char *what)
{
	pid_t child = fork();
	int status;

	if (child == 0)
		_exit(child_fn(fd));
	if (child < 0)
		ksft_exit_fail_msg("%s fork failed\n", what);
	return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	       WEXITSTATUS(status) == 0;
}

static void test_invalid_inputs(int fd)
{
	struct pvsched_create_runner creates[] = {
		{ .tid = (int)gettid(), .flags = 1 },
		{ .tid = (int)gettid(), .runner_id = 1 },
		{ .tid = (int)gettid(), .reserved = { 1, 0 } },
		{ .tid = (int)gettid(), .reserved = { 0, 1 } },
		{ .tid = 0 },
		{ .tid = -1 },
	};
	struct pvsched_query_runner queries[] = {
		{ .runner_id = 0 },
		{ .runner_id = 1, .flags = 1 },
		{ .runner_id = 1, .state = 1 },
		{ .runner_id = 1, .last_fault_errno = -EIO },
		{ .runner_id = 1, .reserved0 = 1 },
		{ .runner_id = 1, .reserved1 = 1 },
	};
	bool valid = true;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(creates); i++)
		valid &= ioctl(fd, PVSCHED_CREATE_RUNNER, &creates[i]) < 0 &&
			 errno == EINVAL;
	ksft_test_result(valid, "CREATE_RUNNER rejects malformed input\n");
	valid = true;
	for (i = 0; i < ARRAY_SIZE(queries); i++)
		valid &= ioctl(fd, PVSCHED_QUERY_RUNNER, &queries[i]) < 0 &&
			 errno == EINVAL;
	ksft_test_result(valid, "QUERY_RUNNER rejects malformed input\n");
}

/* GET_INFO on @fd is refused with EPERM. */
static int info_eperm(int fd)
{
	struct pvsched_info info;

	memset(&info, 0, sizeof(info));
	return ioctl(fd, PVSCHED_GET_INFO, &info) < 0 && errno == EPERM ? 0 : 1;
}

/* An unknown command on @fd is refused with EPERM, not ENOTTY. */
static int unknown_eperm(int fd)
{
	return ioctl(fd, _IO('p', 0xff), NULL) < 0 && errno == EPERM ? 0 : 1;
}

static void test_foreign_fd(int fd)
{
	ksft_test_result(fork_expect(info_eperm, fd, "foreign fd"),
			 "inherited fd cannot issue ioctl from foreign owner\n");
}

static void test_auth_precedence(int fd)
{
	ksft_test_result(fork_expect(unknown_eperm, fd, "auth precedence"),
			 "foreign owner is rejected before unknown-command decoding\n");
}

static void test_control_plane(void)
{
	struct pvsched_create_runner request;
	struct pvsched_query_runner result;
	uint64_t runner_id;
	int fd, other, rc;

	fd = open_session();
	if (fd < 0)
		ksft_exit_fail_msg("control session open failed\n");
	rc = check_info(fd);
	ksft_test_result(rc == 0,
			 "GET_INFO initializes version, limits and reserved fields\n");
	if (rc < 0) {
		close(fd);
		ksft_exit_fail_msg("GET_INFO failed\n");
	}

	rc = create_self(fd, &request);
	ksft_test_result(rc == 0, "CREATE_RUNNER accepts the owner thread\n");
	if (rc) {
		close(fd);
		ksft_exit_fail_msg("owner CREATE_RUNNER failed\n");
	}
	ksft_test_result(request.runner_id != 0 && request.tid == (int)gettid(),
			 "CREATE_RUNNER returns a session-local identity\n");
	runner_id = request.runner_id;
	rc = query(fd, runner_id, &result);
	ksft_test_result(rc == 0 && result.flags == 0 &&
			 result.state == PVSCHED_RUNNER_INACTIVE &&
			 result.last_fault_errno == 0 && !result.reserved0 &&
			 !result.reserved1,
			 "QUERY_RUNNER initializes inactive and fault fields\n");

	/* Duplicate ownership must not mint or reset another runner. */
	memset(&result, 0, sizeof(result));
	rc = create_self(fd, &request);
	ksft_test_result(rc == -EEXIST,
			 "duplicate CREATE_RUNNER is rejected without a second identity\n");
	other = open_session();
	rc = other < 0 ? -errno : create_self(other, &request);
	ksft_test_result(rc == -EEXIST,
			 "runner identity is unique across sessions\n");
	if (other >= 0)
		close(other);

	rc = query(fd, UINT64_MAX, &result);
	ksft_test_result(rc == -ENOENT, "unknown QUERY_RUNNER identity is rejected\n");
	rc = ioctl(fd, _IO('p', 0xff), NULL) < 0 ? -errno : 0;
	ksft_test_result(rc == -ENOTTY,
			 "authorized unknown command returns ENOTTY\n");
	test_invalid_inputs(fd);
	test_foreign_fd(fd);
	test_auth_precedence(fd);
	test_exec_fd();
	{
		int dupfd = dup(fd);
		struct pvsched_query_runner duplicate_query;

		ksft_test_result(dupfd >= 0, "dup shares the pvsched session\n");
		if (dupfd >= 0) {
			close(fd);
			rc = query(dupfd, runner_id, &duplicate_query);
			ksft_test_result(rc == 0, "first close retains the session\n");
			close(dupfd);
		} else {
			close(fd);
		}
	}
}

/* Open a session, then drop privilege: its ioctls are refused. */
static int privilege_loss_child(int unused)
{
	int fd = open("/dev/pvsched", O_RDWR | O_CLOEXEC);

	(void)unused;
	if (fd < 0 || setuid(65534))
		return 2;
	return info_eperm(fd) ?: unknown_eperm(fd);
}

static void test_privilege_loss(void)
{
	ksft_test_result(fork_expect(privilege_loss_child, -1,
				     "privilege loss"),
			 "recognized ioctls recheck current privilege\n");
}

static void test_copyout_rollback(void)
{
	struct pvsched_create_runner *request;
	long page_size = sysconf(_SC_PAGESIZE);
	int fd, rc;

	fd = open_session();
	if (fd < 0)
		ksft_exit_fail_msg("copyout session open failed\n");
	request = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (request == MAP_FAILED)
		ksft_exit_fail_msg("copyout mmap failed\n");
	memset(request, 0, sizeof(*request));
	request->tid = (int)gettid();
	if (mprotect(request, page_size, PROT_READ))
		ksft_exit_fail_msg("copyout mprotect failed\n");
	rc = ioctl(fd, PVSCHED_CREATE_RUNNER, request) < 0 ? -errno : 0;
	ksft_test_result(rc == -EFAULT,
			 "CREATE_RUNNER read-only output returns EFAULT\n");
	if (!mprotect(request, page_size, PROT_READ | PROT_WRITE)) {
		memset(request, 0, sizeof(*request));
		request->tid = (int)gettid();
		rc = ioctl(fd, PVSCHED_CREATE_RUNNER, request) < 0 ? -errno : 0;
		ksft_test_result(rc == 0 && request->runner_id != 0,
				 "copyout failure rolls back ownership for retry\n");
	}
	munmap(request, page_size);
	close(fd);
}

struct worker {
	int fd;
	int rc;
	uint64_t id;
};

static void *create_worker(void *arg)
{
	struct worker *worker = arg;
	struct pvsched_create_runner request;

	worker->rc = create_self(worker->fd, &request);
	worker->id = request.runner_id;
	return NULL;
}

static int create_exited_runner(int fd, uint64_t *id)
{
	struct worker worker = { .fd = fd };
	pthread_t thread;
	int rc;

	rc = pthread_create(&thread, NULL, create_worker, &worker);
	if (rc)
		return -rc;
	rc = pthread_join(thread, NULL);
	if (rc)
		return -rc;
	if (!worker.rc && id)
		*id = worker.id;
	return worker.rc;
}

static void test_exit_and_runner_quota(void)
{
	uint64_t id;
	unsigned int i;
	int fd, rc;

	fd = open_session();
	if (fd < 0)
		ksft_exit_fail_msg("runner quota open failed\n");
	rc = create_exited_runner(fd, &id);
	ksft_test_result(!rc && !wait_exited(fd, id, false),
			 "QUERY reports an exited pthread identity\n");
	for (i = 1; i < limits.max_runners_per_session && !rc; i++)
		rc = create_exited_runner(fd, NULL);
	ksft_test_result(!rc && i == limits.max_runners_per_session,
			 "per-session runner quota can be filled\n");
	if (!rc)
		rc = create_exited_runner(fd, NULL);
	ksft_test_result(rc == -ENOSPC,
			 "per-session runner quota rejects overflow\n");
	close(fd);
}

struct racing_worker {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pid_t tid;
	bool exit;
};

static void *racing_worker(void *arg)
{
	struct racing_worker *worker = arg;

	pthread_mutex_lock(&worker->lock);
	worker->tid = gettid();
	pthread_cond_signal(&worker->cond);
	while (!worker->exit)
		pthread_cond_wait(&worker->cond, &worker->lock);
	pthread_mutex_unlock(&worker->lock);
	return NULL;
}

struct parked_thread {
	pthread_t thread;
	struct racing_worker worker;
};

static pid_t park_thread(struct parked_thread *parked)
{
	parked->worker = (struct racing_worker) {
		.lock = PTHREAD_MUTEX_INITIALIZER,
		.cond = PTHREAD_COND_INITIALIZER,
	};
	if (pthread_create(&parked->thread, NULL, racing_worker,
			   &parked->worker))
		ksft_exit_fail_msg("parked thread create failed\n");
	pthread_mutex_lock(&parked->worker.lock);
	while (!parked->worker.tid)
		pthread_cond_wait(&parked->worker.cond, &parked->worker.lock);
	pthread_mutex_unlock(&parked->worker.lock);
	return parked->worker.tid;
}

/* Let a parked thread exit, without waiting for it. */
static void release_thread(struct parked_thread *parked)
{
	pthread_mutex_lock(&parked->worker.lock);
	parked->worker.exit = true;
	pthread_cond_signal(&parked->worker.cond);
	pthread_mutex_unlock(&parked->worker.lock);
}

static void unpark_thread(struct parked_thread *parked)
{
	release_thread(parked);
	pthread_join(parked->thread, NULL);
}

static void test_create_exit_race(void)
{
	bool valid = true;
	unsigned int i;
	int fd;

	fd = open_session();
	if (fd < 0)
		ksft_exit_fail_msg("create/exit race session open failed\n");
	for (i = 0; i < 100 && valid; i++) {
		struct parked_thread parked;
		uint64_t id = 0;
		pid_t tid;
		int rc;

		/* The runner is created while the thread is exiting. */
		tid = park_thread(&parked);
		release_thread(&parked);
		rc = create_tid(fd, tid, &id);
		if (pthread_join(parked.thread, NULL)) {
			valid = false;
			break;
		}
		valid = rc == -ESRCH || (!rc && id &&
			!wait_exited(fd, id, false));
	}
	ksft_test_result(valid, "CREATE_RUNNER safely races target exit\n");
	close(fd);
}

static void test_sigkill_cleanup(void)
{
	int ready[2], status;
	char byte;
	pid_t child;

	if (pipe(ready))
		ksft_exit_fail_msg("SIGKILL cleanup pipe failed\n");
	child = fork();
	if (!child) {
		struct pvsched_create_runner request;
		int fd = open_session();

		close(ready[0]);
		if (fd < 0 || create_self(fd, &request) ||
		    write(ready[1], "x", 1) != 1)
			_exit(1);
		pause();
		_exit(1);
	}
	close(ready[1]);
	if (child < 0 || read(ready[0], &byte, 1) != 1)
		ksft_exit_fail_msg("SIGKILL cleanup child setup failed\n");
	kill(child, SIGKILL);
	ksft_test_result(waitpid(child, &status, 0) == child &&
			 WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
			 "SIGKILL closes an owned runner session\n");
	close(ready[0]);
}

static void test_session_quota(void)
{
	unsigned int i, max = limits.max_sessions_global;
	int *fds = calloc(max, sizeof(*fds));
	int extra = -1;

	if (!fds)
		ksft_exit_fail_msg("session quota allocation failed\n");
	for (i = 0; i < max; i++) {
		fds[i] = open("/dev/pvsched", O_RDWR | O_CLOEXEC);
		if (fds[i] < 0)
			break;
	}
	ksft_test_result(i == max, "global session quota can be filled\n");
	if (i == max)
		extra = open("/dev/pvsched", O_RDWR | O_CLOEXEC);
	ksft_test_result(extra < 0 && errno == ENOSPC,
			 "global session quota rejects overflow\n");
	if (extra >= 0)
		close(extra);
	while (i)
		close(fds[--i]);
	free(fds);
}

static void test_global_runner_quota(void)
{
	unsigned int nr_sessions = limits.max_runners_global /
				   limits.max_runners_per_session + 1;
	int *fds = calloc(nr_sessions, sizeof(*fds));
	unsigned int i;
	int rc = 0;

	if (!fds)
		ksft_exit_fail_msg("global runner quota allocation failed\n");
	for (i = 0; i < nr_sessions; i++) {
		fds[i] = open_session();
		if (fds[i] < 0)
			ksft_exit_fail_msg("global runner quota open failed\n");
	}
	for (i = 0; i < limits.max_runners_global && !rc; i++)
		rc = create_exited_runner(fds[i % nr_sessions], NULL);
	ksft_test_result(!rc && i == limits.max_runners_global,
			 "global runner quota can be filled\n");
	if (!rc)
		rc = create_exited_runner(fds[0], NULL);
	ksft_test_result(rc == -ENOSPC,
			 "global runner quota rejects overflow\n");
	for (i = 0; i < nr_sessions; i++)
		close(fds[i]);
	free(fds);
}

static void test_final_close(void)
{
	struct pvsched_create_runner request;
	int fd, rc;

	fd = open_session();
	rc = fd < 0 ? -errno : create_self(fd, &request);
	if (fd >= 0)
		close(fd);
	fd = open_session();
	if (fd < 0)
		rc = -errno;
	else if (!rc)
		rc = create_self(fd, &request);
	ksft_test_result(!rc, "final close releases runner ownership\n");
	if (fd >= 0)
		close(fd);
}

/* Hand a session to a fresh image of this test, which must be refused. */
static int query_policy(int fd, uint32_t index, struct pvsched_policy_info *info)
{
	memset(info, 0, sizeof(*info));
	info->index = index;
	return ioctl(fd, PVSCHED_QUERY_POLICY, info) < 0 ? -errno : 0;
}

static void test_policy_listing(void)
{
	struct pvsched_policy_info info, bad;
	int fd = open("/dev/pvsched", O_RDWR | O_CLOEXEC);
	bool ok = true;
	uint32_t i;

	if (fd < 0)
		ksft_exit_fail_msg("policy session open failed\n");
	ksft_test_result(!query_policy(fd, 0, &info) &&
			 !strcmp(info.name, PVSCHED_DEFAULT_POLICY_NAME) &&
			 info.version == PVSCHED_DEFAULT_POLICY_VERSION &&
			 info.protocol == PVSCHED_PROTOCOL_DEFAULT &&
			 info.params_size && !info.flags && !info.reserved,
			 "QUERY_POLICY lists the default policy first\n");

	/* Every input field but the index is zero. */
	memset(&bad, 0, sizeof(bad));
	bad.flags = 1;
	ok &= ioctl(fd, PVSCHED_QUERY_POLICY, &bad) < 0 && errno == EINVAL;
	memset(&bad, 0, sizeof(bad));
	bad.name[0] = 'x';
	ok &= ioctl(fd, PVSCHED_QUERY_POLICY, &bad) < 0 && errno == EINVAL;
	memset(&bad, 0, sizeof(bad));
	bad.version = 1;
	ok &= ioctl(fd, PVSCHED_QUERY_POLICY, &bad) < 0 && errno == EINVAL;
	memset(&bad, 0, sizeof(bad));
	bad.reserved = 1;
	ok &= ioctl(fd, PVSCHED_QUERY_POLICY, &bad) < 0 && errno == EINVAL;
	for (i = 0; i < 1024 && !query_policy(fd, i, &info); i++)
		;
	ok &= i < 1024 && query_policy(fd, i, &info) == -ENOENT;
	ksft_test_result(ok, "QUERY_POLICY checks its input and ends with ENOENT\n");
	close(fd);
}

static void test_policy_selection(void)
{
	struct pvsched_create_runner request;
	struct pvsched_set_policy set;
	int fd = open("/dev/pvsched", O_RDWR | O_CLOEXEC);
	bool ok = true;

	if (fd < 0)
		ksft_exit_fail_msg("policy session open failed\n");
	ksft_test_result(create_self(fd, &request) == -EINVAL,
			 "CREATE_RUNNER requires SET_POLICY first\n");

	memset(&set, 0, sizeof(set));
	set.version = PVSCHED_DEFAULT_POLICY_VERSION;
	ok &= ioctl(fd, PVSCHED_SET_POLICY, &set) < 0 && errno == EINVAL;
	strcpy(set.name, PVSCHED_DEFAULT_POLICY_NAME);
	set.name[sizeof(set.name) - 1] = 'x';
	ok &= ioctl(fd, PVSCHED_SET_POLICY, &set) < 0 && errno == EINVAL;
	set.name[sizeof(set.name) - 1] = 0;
	set.flags = 1;
	ok &= ioctl(fd, PVSCHED_SET_POLICY, &set) < 0 && errno == EINVAL;
	ok &= set_policy(fd, "no-such-policy", 1) == -ENOENT;
	ok &= set_policy(fd, PVSCHED_DEFAULT_POLICY_NAME,
			 PVSCHED_DEFAULT_POLICY_VERSION + 1) == -ENOENT;
	ksft_test_result(ok, "SET_POLICY rejects malformed and unknown policies\n");

	ok = !set_policy(fd, PVSCHED_DEFAULT_POLICY_NAME,
			 PVSCHED_DEFAULT_POLICY_VERSION);
	ok &= set_policy(fd, PVSCHED_DEFAULT_POLICY_NAME,
			 PVSCHED_DEFAULT_POLICY_VERSION) == -EBUSY;
	ok &= !create_self(fd, &request);
	ksft_test_result(ok, "SET_POLICY is allowed once, before CREATE_RUNNER\n");
	close(fd);
}

static void test_policies(void)
{
	test_policy_listing();
	test_policy_selection();
}

static int exec_fd_child(int unused)
{
	const char *self = "/proc/self/exe";
	int fd = open("/dev/pvsched", O_RDWR);

	(void)unused;
	if (fd < 0 || (fd != 100 && dup3(fd, 100, 0) < 0))
		return 110;
	if (fd != 100)
		close(fd);
	execl(self, self, "--query-fd", "100", NULL);
	return 111;
}

static void test_exec_fd(void)
{
	ksft_test_result(fork_expect(exec_fd_child, -1, "exec fixture"),
			 "post-exec inherited fd cannot issue ioctl\n");
}

int main(int argc, char **argv)
{
	int original_nice, original_policy;

	if (argc == 3 && !strcmp(argv[1], "--query-fd"))
		return info_eperm(atoi(argv[2]));
	ksft_print_header();
	ksft_set_plan(34);
	{
		int probe = open("/dev/pvsched", O_RDWR | O_CLOEXEC);

		if (probe < 0) {
			if (errno == ENOENT || errno == ENODEV || errno == EACCES ||
			    errno == EPERM)
				ksft_exit_skip("/dev/pvsched unavailable: %s\n",
					       strerror(errno));
			ksft_exit_fail_msg("unexpected device open failure: %s\n",
					   strerror(errno));
		}
		close(probe);
	}
	alarm(300);
	original_policy = sched_getscheduler(0);
	errno = 0;
	original_nice = getpriority(PRIO_PROCESS, 0);
	test_control_plane();
	test_privilege_loss();
	test_copyout_rollback();
	test_exit_and_runner_quota();
	test_create_exit_race();
	test_sigkill_cleanup();
	test_session_quota();
	test_global_runner_quota();
	test_final_close();
	test_policies();
	ksft_test_result(original_policy == sched_getscheduler(0) &&
			 original_nice == getpriority(PRIO_PROCESS, 0),
			 "inactive control leaves caller scheduling unchanged\n");
	alarm(0);
	ksft_finished();
}
