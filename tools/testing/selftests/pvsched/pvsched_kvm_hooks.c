// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_modules/hook_observer.h"

#define MEM_SIZE (2U * 1024 * 1024)
#define PD_ADDR 0x2000
#define TIMEOUT_SEC 5
#define PLAN 10
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static int number, failures;

struct vm {
	int kvm, vm, vcpu;
	struct kvm_run *run;
	size_t run_size;
	void *mem;
};

static void result(int ok, const char *name)
{
	printf("%s %d - %s\n", ok ? "ok" : "not ok", ++number, name);
	if (!ok)
		failures++;
}

static void skip(const char *name, const char *why)
{
	printf("ok %d - %s # SKIP %s\n", ++number, name, why);
}

static void flat_segment(struct kvm_segment *seg, uint16_t selector,
			 uint8_t type)
{
	memset(seg, 0, sizeof(*seg));
	seg->limit = 0xfffff;
	seg->selector = selector;
	seg->type = type;
	seg->present = 1;
	seg->s = 1;
	seg->db = 1;
	seg->g = 1;
}

static void vm_close(struct vm *v)
{
	if (v->run && v->run != MAP_FAILED)
		munmap(v->run, v->run_size);
	if (v->vcpu >= 0)
		close(v->vcpu);
	if (v->mem && v->mem != MAP_FAILED)
		munmap(v->mem, MEM_SIZE);
	if (v->vm >= 0)
		close(v->vm);
	if (v->kvm >= 0)
		close(v->kvm);
}

static int vm_init(struct vm *v, int irqchip)
{
	struct kvm_userspace_memory_region region = { };
	struct kvm_sregs sregs;
	struct kvm_regs regs = { .rip = 0, .rflags = 2 };
	struct kvm_lapic_state lapic;
	long page_size = sysconf(_SC_PAGESIZE);
	int ret;

	*v = (struct vm) { .kvm = -1, .vm = -1, .vcpu = -1 };
	v->kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (v->kvm < 0)
		return -errno;
	if (ioctl(v->kvm, KVM_GET_API_VERSION, 0) != KVM_API_VERSION)
		return -EINVAL;
	v->vm = ioctl(v->kvm, KVM_CREATE_VM, 0);
	if (v->vm < 0)
		return -errno;
	if (irqchip && ioctl(v->vm, KVM_CREATE_IRQCHIP, 0) < 0)
		return -errno;
	v->mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (v->mem == MAP_FAILED)
		return -errno;
	((uint8_t *)v->mem)[0] = 0xfb; /* sti */
	((uint8_t *)v->mem)[1] = 0xf4; /* hlt */
	*(uint32_t *)((uint8_t *)v->mem + PD_ADDR) = 0x83;
	region.memory_size = MEM_SIZE;
	region.userspace_addr = (uintptr_t)v->mem;
	if (ioctl(v->vm, KVM_SET_USER_MEMORY_REGION, &region) < 0)
		return -errno;
	v->vcpu = ioctl(v->vm, KVM_CREATE_VCPU, 0);
	if (v->vcpu < 0)
		return -errno;
	if (ioctl(v->vcpu, KVM_GET_SREGS, &sregs) < 0)
		return -errno;
	sregs.cr3 = PD_ADDR;
	sregs.cr4 |= 0x10;
	sregs.cr0 |= 0x80000001;
	flat_segment(&sregs.cs, 0x08, 0x0b);
	flat_segment(&sregs.ds, 0x10, 0x03);
	sregs.es = sregs.ds;
	sregs.fs = sregs.ds;
	sregs.gs = sregs.ds;
	sregs.ss = sregs.ds;
	if (ioctl(v->vcpu, KVM_SET_SREGS, &sregs) < 0 ||
	    ioctl(v->vcpu, KVM_SET_REGS, &regs) < 0)
		return -errno;
	if (irqchip) {
		if (ioctl(v->vcpu, KVM_GET_LAPIC, &lapic) < 0)
			return -errno;
		*(uint32_t *)(lapic.regs + 0xf0) = 0x1ff;
		if (ioctl(v->vcpu, KVM_SET_LAPIC, &lapic) < 0)
			return -errno;
	}
	ret = ioctl(v->kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (ret < page_size)
		return ret < 0 ? -errno : -EINVAL;
	v->run_size = ret;
	v->run = mmap(NULL, ret, PROT_READ | PROT_WRITE, MAP_SHARED, v->vcpu, 0);
	return v->run == MAP_FAILED ? -errno : 0;
}

static int reset_rip(struct vm *v)
{
	struct kvm_regs regs;

	if (ioctl(v->vcpu, KVM_GET_REGS, &regs) < 0)
		return -1;
	regs.rip = 0;
	regs.rflags = 2;
	return ioctl(v->vcpu, KVM_SET_REGS, &regs);
}

static int snapshot(int fd, struct pvsched_hook_snapshot *s)
{
	return pread(fd, s, sizeof(*s), 0) == sizeof(*s) ? 0 : -1;
}

/* Mode bits that kvm_amd's AVIC or vNMI setting reports for every VM. */
static uint32_t host_mode_flags;

static int kvm_amd_param_on(const char *name)
{
	char path[64], value = 0;
	int fd;

	snprintf(path, sizeof(path), "/sys/module/kvm_amd/parameters/%s", name);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	if (read(fd, &value, 1) != 1)
		value = 0;
	close(fd);
	return value == 'Y' || value == '1';
}

/* The VM reports no mode of its own beyond the host's configuration. */
static int no_vm_mode(uint32_t mode_flags)
{
	return !(mode_flags & ~host_mode_flags);
}

static int zero_record(const struct pvsched_hook_record *record,
		       unsigned int seq, uint32_t event, uint32_t event_flags)
{
	return record->seq == seq && record->event == event &&
		!record->vcpu_id && !record->ret && !record->exit_reason &&
		no_vm_mode(record->mode_flags) && record->event_flags == event_flags;
}

static int run_records(const struct pvsched_hook_snapshot *s, unsigned int first,
		       unsigned int end, int ret, uint32_t reason)
{
	const struct pvsched_hook_record *record;
	unsigned int i = first, vmexits = 0;
	bool common_cancel = false;
	bool vendor_open = false;

	if (end - first < 2 ||
	    !zero_record(&s->records[i], i, PVSCHED_HOOK_ENTER, 0))
		return 0;
	i++;
	while (i + 1 < end) {
		if (s->records[i].event == PVSCHED_HOOK_VMENTRY) {
			record = &s->records[i];
			if (record->event_flags &
			    ~PVSCHED_HOOK_RECORD_F_INTERRUPT_READY ||
			    !zero_record(record, i, PVSCHED_HOOK_VMENTRY,
					 record->event_flags))
				return 0;
			vendor_open = true;
			i++;
			continue;
		}

		/* An early common cancellation has no IRQ-off terminal fact. */
		if (s->records[i].event == PVSCHED_HOOK_VMENTRY_CANCEL) {
			if (vendor_open ||
			    !zero_record(&s->records[i], i,
					 PVSCHED_HOOK_VMENTRY_CANCEL, 0))
				return 0;
			common_cancel = true;
			i++;
			continue;
		}

		if (s->records[i].event != PVSCHED_HOOK_VMEXIT_IRQOFF ||
		    !zero_record(&s->records[i], i,
				 PVSCHED_HOOK_VMEXIT_IRQOFF, 0))
			return 0;
		i++;
		/* Only the HLT-exit fact may be set on the IRQ-on terminal fact. */
		record = &s->records[i];
		if (i + 1 >= end ||
		    record->event_flags & ~PVSCHED_HOOK_RECORD_F_HLT_EXIT ||
		    !zero_record(record, i, PVSCHED_HOOK_VMEXIT,
				 record->event_flags))
			return 0;
		vmexits++;
		vendor_open = false;
		i++;
	}

	record = &s->records[end - 1];
	return i == end - 1 && !vendor_open &&
		(vmexits || common_cancel) && record->seq == end - 1 &&
		record->event == PVSCHED_HOOK_LEAVE && !record->vcpu_id &&
		no_vm_mode(record->mode_flags) && !record->event_flags &&
		record->ret == ret && (ret || record->exit_reason == reason);
}

struct grammar_event {
	uint32_t event;
	uint32_t flags;
};

#define GRAMMAR_EVENT(_event) { .event = (_event) }
#define GRAMMAR_EVENT_FLAGS(_event, _flags) { .event = (_event), .flags = (_flags) }
static int grammar_vector(const struct grammar_event *events, size_t count)
{
	struct pvsched_hook_snapshot s = { .count = count, .next_seq = count };
	size_t i;

	if (count > ARRAY_SIZE(s.records))
		return 0;
	for (i = 0; i < count; i++) {
		s.records[i].seq = i;
		s.records[i].event = events[i].event;
		s.records[i].event_flags = events[i].flags;
	}
	s.records[count - 1].exit_reason = KVM_EXIT_HLT;
	return run_records(&s, 0, count, 0, KVM_EXIT_HLT);
}

/* A userspace HLT exit reports exactly its final VMEXIT as an HLT exit. */
static bool run_ends_in_hlt_exit(const struct pvsched_hook_snapshot *s,
				 unsigned int first, unsigned int end)
{
	unsigned int i, hlt_exits = 0;

	for (i = first; i < end; i++)
		if (s->records[i].event == PVSCHED_HOOK_VMEXIT &&
		    s->records[i].event_flags & PVSCHED_HOOK_RECORD_F_HLT_EXIT)
			hlt_exits++;
	return hlt_exits == 1 && end - first >= 2 &&
		s->records[end - 2].event == PVSCHED_HOOK_VMEXIT &&
		s->records[end - 2].event_flags & PVSCHED_HOOK_RECORD_F_HLT_EXIT;
}

static bool run_has_vmentry(const struct pvsched_hook_snapshot *s,
			    unsigned int first, unsigned int end)
{
	unsigned int i;

	for (i = first; i < end; i++)
		if (s->records[i].event == PVSCHED_HOOK_VMENTRY)
			return true;
	return false;
}

static int event_grammar_vectors(void)
{
	static const struct grammar_event synthetic[] = {
		GRAMMAR_EVENT(PVSCHED_HOOK_ENTER),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMEXIT_IRQOFF),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMEXIT),
		GRAMMAR_EVENT(PVSCHED_HOOK_LEAVE),
	};
	static const struct grammar_event fast[] = {
		GRAMMAR_EVENT(PVSCHED_HOOK_ENTER),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMENTRY),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMENTRY),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMEXIT_IRQOFF),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMEXIT),
		GRAMMAR_EVENT(PVSCHED_HOOK_LEAVE),
	};
	static const struct grammar_event early_cancel[] = {
		GRAMMAR_EVENT(PVSCHED_HOOK_ENTER),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMENTRY_CANCEL),
		GRAMMAR_EVENT(PVSCHED_HOOK_LEAVE),
	};
	static const struct grammar_event dangling_gate[] = {
		GRAMMAR_EVENT(PVSCHED_HOOK_ENTER),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMENTRY),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMEXIT_IRQOFF),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMEXIT),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMENTRY),
		GRAMMAR_EVENT(PVSCHED_HOOK_LEAVE),
	};
	static const struct grammar_event hlt_exit[] = {
		GRAMMAR_EVENT(PVSCHED_HOOK_ENTER),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMENTRY),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMEXIT_IRQOFF),
		GRAMMAR_EVENT_FLAGS(PVSCHED_HOOK_VMEXIT,
				    PVSCHED_HOOK_RECORD_F_HLT_EXIT),
		GRAMMAR_EVENT(PVSCHED_HOOK_LEAVE),
	};
	static const struct grammar_event unknown_exit_fact[] = {
		GRAMMAR_EVENT(PVSCHED_HOOK_ENTER),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMENTRY),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMEXIT_IRQOFF),
		GRAMMAR_EVENT_FLAGS(PVSCHED_HOOK_VMEXIT,
				    PVSCHED_HOOK_RECORD_F_INTERRUPT_READY),
		GRAMMAR_EVENT(PVSCHED_HOOK_LEAVE),
	};
	static const struct grammar_event mismatched_cancel[] = {
		GRAMMAR_EVENT(PVSCHED_HOOK_ENTER),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMENTRY),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMENTRY_CANCEL),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMEXIT_IRQOFF),
		GRAMMAR_EVENT(PVSCHED_HOOK_VMEXIT),
		GRAMMAR_EVENT(PVSCHED_HOOK_LEAVE),
	};

	return grammar_vector(synthetic, ARRAY_SIZE(synthetic)) &&
		grammar_vector(fast, ARRAY_SIZE(fast)) &&
		grammar_vector(early_cancel, ARRAY_SIZE(early_cancel)) &&
		grammar_vector(hlt_exit, ARRAY_SIZE(hlt_exit)) &&
		!grammar_vector(unknown_exit_fact, ARRAY_SIZE(unknown_exit_fact)) &&
		!grammar_vector(dangling_gate, ARRAY_SIZE(dangling_gate)) &&
		!grammar_vector(mismatched_cancel, ARRAY_SIZE(mismatched_cancel));
}

static int normal_run_facts(const struct pvsched_hook_snapshot *s,
			    int ret, uint32_t reason)
{
	if (s->count < 4 || s->flags || s->next_seq != s->count ||
	    !run_records(s, 0, s->count, ret, reason))
		return 0;
	return run_has_vmentry(s, 0, s->count) &&
		(ret || reason != KVM_EXIT_HLT ||
		 run_ends_in_hlt_exit(s, 0, s->count));
}

static int repeated_run_facts(const struct pvsched_hook_snapshot *s)
{
	unsigned int second;

	if (s->count < 10 || s->flags || s->next_seq != s->count)
		return 0;
	for (second = 1; second < s->count; second++) {
		if (s->records[second - 1].event == PVSCHED_HOOK_LEAVE)
			break;
	}
	return second < s->count &&
		run_has_vmentry(s, 0, second) &&
		run_has_vmentry(s, second, s->count) &&
		run_records(s, 0, second, 0, KVM_EXIT_HLT) &&
		run_records(s, second, s->count, 0, KVM_EXIT_HLT) &&
		run_ends_in_hlt_exit(s, 0, second) &&
		run_ends_in_hlt_exit(s, second, s->count);
}

static int halt_unhalt_facts(const struct pvsched_hook_snapshot *s)
{
	unsigned int i, pairs = 0;

	if (s->flags || s->count < 4 ||
	    s->records[0].event != PVSCHED_HOOK_ENTER ||
	    s->records[s->count - 1].event != PVSCHED_HOOK_LEAVE)
		return 0;
	for (i = 0; i < s->count; i++) {
		const struct pvsched_hook_record *record = &s->records[i];

		if (record->seq != i || record->vcpu_id ||
		    !no_vm_mode(record->mode_flags))
			return 0;
		if (record->event == PVSCHED_HOOK_HALT) {
			if (record->event_flags || i + 1 >= s->count ||
			    !zero_record(&s->records[i + 1], i + 1,
					 PVSCHED_HOOK_UNHALT, 0))
				return 0;
			pairs++;
			i++;
		} else if (record->event == PVSCHED_HOOK_UNHALT) {
			return 0;
		}
	}
	return pairs > 0;
}

static int queued_nmi_fact(const struct pvsched_hook_snapshot *s)
{
	const struct pvsched_hook_record *record = s->records;

	return s->count == 1 && !s->flags && s->next_seq == 1 &&
		record->event == PVSCHED_HOOK_INJECT_INTR && !record->seq &&
		!record->vcpu_id && !record->ret && !record->exit_reason &&
		no_vm_mode(record->mode_flags) && !record->event_flags;
}

static int child_wrong_mm(struct vm *inherited)
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return 0;
	if (!pid) {
		struct pvsched_hook_snapshot s;
		struct vm own;
		int obs, ret, ok;

		alarm(20);
		obs = open("/dev/" PVSCHED_HOOK_OBSERVER_DEVICE, O_RDWR | O_CLOEXEC);
		if (obs < 0 || vm_init(&own, 0))
			_exit(2);
		ret = ioctl(own.vcpu, KVM_RUN, 0);
		ok = ret == 0 && own.run->exit_reason == KVM_EXIT_HLT &&
		     !snapshot(obs, &s) &&
		     normal_run_facts(&s, 0, KVM_EXIT_HLT);
		ok &= !ioctl(obs, PVSCHED_HOOK_IOC_RESET, 0);
		errno = 0;
		ret = ioctl(inherited->vcpu, KVM_RUN, 0);
		ok &= ret < 0 && errno == EIO && !snapshot(obs, &s) &&
		      !s.count && !s.flags;
		vm_close(&own);
		close(obs);
		_exit(ok ? 0 : 1);
	}
	return waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
	       !WEXITSTATUS(status);
}

struct stats {
	int fd;
	off_t blocking, halt_wait;
};

static int stats_init(int vcpu, struct stats *st)
{
	struct kvm_stats_header h;
	struct kvm_stats_desc *d;
	size_t stride, bytes;
	char *all, *name;
	unsigned int i;

	*st = (struct stats) { .fd = -1, .blocking = -1, .halt_wait = -1 };
	st->fd = ioctl(vcpu, KVM_GET_STATS_FD, 0);
	if (st->fd < 0)
		return -errno;
	if (pread(st->fd, &h, sizeof(h), 0) != sizeof(h) || !h.name_size ||
	    h.num_desc > 4096) {
		close(st->fd);
		st->fd = -1;
		return -EINVAL;
	}
	stride = sizeof(*d) + h.name_size;
	bytes = stride * h.num_desc;
	all = malloc(bytes);
	if (!all) {
		close(st->fd);
		st->fd = -1;
		return -ENOMEM;
	}
	if (pread(st->fd, all, bytes, h.desc_offset) != (ssize_t)bytes) {
		free(all);
		close(st->fd);
		st->fd = -1;
		return -EIO;
	}
	for (i = 0; i < h.num_desc; i++) {
		d = (void *)(all + i * stride);
		name = all + i * stride + sizeof(*d);
		if (!memchr(name, 0, h.name_size) || d->size != 1)
			continue;
		if (!strcmp(name, "blocking"))
			st->blocking = h.data_offset + d->offset;
		else if (!strcmp(name, "halt_wait_ns"))
			st->halt_wait = h.data_offset + d->offset;
	}
	free(all);
	if (st->blocking < 0) {
		close(st->fd);
		st->fd = -1;
		return -ENOENT;
	}
	return 0;
}

static int stat_read(struct stats *st, off_t offset, uint64_t *value)
{
	return pread(st->fd, value, sizeof(*value), offset) == sizeof(*value) ?
		0 : -EIO;
}

struct blocked {
	struct vm *vm;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int ready, observer, ret, error;
	struct pvsched_hook_snapshot snapshot;
};

static void handle_signal(int sig) { (void)sig; }

static void *blocked_run(void *arg)
{
	struct blocked *b = arg;
	sigset_t signals;

	sigemptyset(&signals);
	sigaddset(&signals, SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &signals, NULL);
	b->observer = open("/dev/" PVSCHED_HOOK_OBSERVER_DEVICE,
			   O_RDWR | O_CLOEXEC);
	pthread_mutex_lock(&b->lock);
	b->ready = b->observer < 0 ? -errno : 1;
	pthread_cond_signal(&b->cond);
	pthread_mutex_unlock(&b->lock);
	if (b->observer < 0)
		return NULL;
	errno = 0;
	b->ret = ioctl(b->vm->vcpu, KVM_RUN, 0);
	b->error = errno;
	if (snapshot(b->observer, &b->snapshot))
		b->error = EPROTO;
	close(b->observer);
	return NULL;
}

static int blocked_signal(struct vm *vm, int *unsupported)
{
	struct blocked b = { .vm = vm, .observer = -1 };
	struct stats st;
	struct sigaction sa = { .sa_handler = handle_signal };
	struct timespec deadline;
	pthread_t thread;
	uint64_t blocking = 0, before = 0, after = 0;
	int joined = 0, ok = 0, ret;

	ret = stats_init(vm->vcpu, &st);
	if (ret) {
		*unsupported = ret == -ENOTTY;
		return 0;
	}
	*unsupported = 0;
	if (st.halt_wait >= 0 && stat_read(&st, st.halt_wait, &before)) {
		close(st.fd);
		return 0;
	}
	pthread_mutex_init(&b.lock, NULL);
	pthread_cond_init(&b.cond, NULL);
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	if (pthread_create(&thread, NULL, blocked_run, &b))
		goto out;
	pthread_mutex_lock(&b.lock);
	while (!b.ready)
		pthread_cond_wait(&b.cond, &b.lock);
	pthread_mutex_unlock(&b.lock);
	if (b.ready < 0)
		goto stop;
	if (ioctl(b.observer, PVSCHED_HOOK_IOC_WAIT_HALT, 0) ||
	    pthread_kill(thread, SIGUSR1))
		goto stop;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += TIMEOUT_SEC;
	if (pthread_timedjoin_np(thread, NULL, &deadline))
		goto stop;
	joined = 1;
	if (stat_read(&st, st.blocking, &blocking))
		goto out;
	if (st.halt_wait >= 0 && stat_read(&st, st.halt_wait, &after))
		goto out;
	ok = b.ret < 0 && b.error == EINTR && !blocking &&
	     halt_unhalt_facts(&b.snapshot) &&
	     (st.halt_wait < 0 || after > before);
	goto out;
stop:
	pthread_kill(thread, SIGUSR1);
	if (!joined)
		pthread_join(thread, NULL);
out:
	close(st.fd);
	pthread_cond_destroy(&b.cond);
	pthread_mutex_destroy(&b.lock);
	return ok;
}

int main(void)
{
	struct pvsched_hook_snapshot s;
	struct vm vm, blocked;
	struct sched_param sp1, sp2;
	int observer, ret, unsupported, pol1, pol2, nice1, nice2, sched_ok;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("TAP version 13\n");
	if (kvm_amd_param_on("avic"))
		host_mode_flags |= PVSCHED_HOOK_MODE_SVM_AVIC;
	if (kvm_amd_param_on("vnmi"))
		host_mode_flags |= PVSCHED_HOOK_MODE_SVM_VNMI;
	alarm(60);
	observer = open("/dev/" PVSCHED_HOOK_OBSERVER_DEVICE,
			O_RDWR | O_CLOEXEC);
	if (observer < 0 && (errno == ENOENT || errno == ENODEV)) {
		printf("1..0 # SKIP hook observer module is not loaded\n");
		return 4;
	}
	if (observer < 0) {
		printf("Bail out! observer open failed: %s\n", strerror(errno));
		return 1;
	}
	ret = vm_init(&vm, 0);
	if (ret == -ENOENT || ret == -ENODEV || ret == -EACCES || ret == -EPERM) {
		printf("1..0 # SKIP KVM is unavailable: %s\n", strerror(-ret));
		close(observer);
		vm_close(&vm);
		return 4;
	}
	if (ret) {
		printf("Bail out! KVM fixture setup failed: %s\n", strerror(-ret));
		close(observer);
		vm_close(&vm);
		return 1;
	}
	printf("1..%d\n", PLAN);
	result(event_grammar_vectors(),
	       "event grammar accepts legal return and common cancellation forms");
	pol1 = sched_getscheduler(0);
	sched_ok = pol1 >= 0 && !sched_getparam(0, &sp1);
	errno = 0;
	nice1 = getpriority(PRIO_PROCESS, 0);
	sched_ok &= nice1 != -1 || !errno;
	ret = ioctl(observer, PVSCHED_HOOK_IOC_RESET, 0) || reset_rip(&vm);
	ret = ret ?: ioctl(vm.vcpu, KVM_RUN, 0);
	result(!ret && vm.run->exit_reason == KVM_EXIT_HLT &&
	       !snapshot(observer, &s) && normal_run_facts(&s, 0, KVM_EXIT_HLT),
	       "normal HLT run has exact factual event order and arguments");

	ret = ioctl(observer, PVSCHED_HOOK_IOC_RESET, 0) || reset_rip(&vm);
	ret = ret ?: ioctl(vm.vcpu, KVM_RUN, 0);
	ret = ret ?: reset_rip(&vm);
	ret = ret ?: ioctl(vm.vcpu, KVM_RUN, 0);
	result(!ret && !snapshot(observer, &s) && repeated_run_facts(&s),
	       "repeated runs remain sequential and balanced");

	ret = ioctl(vm.kvm, KVM_CHECK_EXTENSION, KVM_CAP_IMMEDIATE_EXIT);
	if (!ret) {
		skip("immediate_exit has a pair", "capability unavailable");
	} else if (ret < 0) {
		result(0, "query immediate_exit capability");
	} else {
		ret = ioctl(observer, PVSCHED_HOOK_IOC_RESET, 0);
		vm.run->immediate_exit = 1;
		errno = 0;
		if (!ret)
			ret = ioctl(vm.vcpu, KVM_RUN, 0);
		vm.run->immediate_exit = 0;
		result(ret < 0 && errno == EINTR && !snapshot(observer, &s) &&
		       s.count == 2 && !s.flags && s.next_seq == 2 &&
		       s.records[0].seq == 0 && s.records[0].event == PVSCHED_HOOK_ENTER &&
		       !s.records[0].vcpu_id && no_vm_mode(s.records[0].mode_flags) &&
		       !s.records[0].event_flags && s.records[1].seq == 1 &&
		       s.records[1].event == PVSCHED_HOOK_LEAVE &&
		       !s.records[1].vcpu_id && no_vm_mode(s.records[1].mode_flags) &&
		       !s.records[1].event_flags && s.records[1].ret == -EINTR,
		       "immediate_exit preserves the outer RUN pair");
	}

	ret = ioctl(observer, PVSCHED_HOOK_IOC_RESET, 0);
	errno = 0;
	if (!ret)
		ret = ioctl(vm.vcpu, KVM_RUN, 1);
	result(ret < 0 && errno == EINVAL && !snapshot(observer, &s) &&
	       !s.count && !s.flags,
	       "invalid generic RUN argument emits no architecture event");

	ret = ioctl(observer, PVSCHED_HOOK_IOC_RESET, 0);
	vm.run->kvm_valid_regs = 1ULL << 63;
	errno = 0;
	if (!ret)
		ret = ioctl(vm.vcpu, KVM_RUN, 0);
	vm.run->kvm_valid_regs = 0;
	result(ret < 0 && errno == EINVAL && !snapshot(observer, &s) &&
	       s.count == 2 && !s.flags && s.next_seq == 2 &&
	       s.records[0].seq == 0 && s.records[0].event == PVSCHED_HOOK_ENTER &&
	       s.records[1].seq == 1 && s.records[1].event == PVSCHED_HOOK_LEAVE &&
	       s.records[1].ret == -EINVAL,
	       "invalid x86 run state has a complete error pair");

	ret = ioctl(observer, PVSCHED_HOOK_IOC_RESET, 0);
	if (!ret)
		ret = ioctl(vm.vcpu, KVM_GET_REGS, &(struct kvm_regs) { });
	pol2 = sched_getscheduler(0);
	sched_ok &= pol2 >= 0 && !sched_getparam(0, &sp2);
	errno = 0;
	nice2 = getpriority(PRIO_PROCESS, 0);
	sched_ok &= nice2 != -1 || !errno;
	result(sched_ok && !ret && !snapshot(observer, &s) && !s.count && !s.flags &&
	       pol1 == pol2 && sp1.sched_priority == sp2.sched_priority &&
	       nice1 == nice2,
	       "non-RUN ioctl emits nothing and scheduling remains unchanged");

	close(observer);
	result(child_wrong_mm(&vm),
	       "child own VM runs but inherited vCPU wrong-mm call emits nothing");

	ret = ioctl(vm.kvm, KVM_CHECK_EXTENSION, KVM_CAP_IRQCHIP);
	if (!ret) {
		skip("signal balances a HLT block attempt", "irqchip unavailable");
	} else if (ret < 0) {
		result(0, "query in-kernel irqchip capability");
	} else {
		ret = vm_init(&blocked, 1);
		if (!ret)
			ret = blocked_signal(&blocked, &unsupported);
		if (!ret && unsupported)
			skip("signal balances a HLT block attempt",
			     "binary vCPU statistics unavailable");
		else
			result(ret == 1, "signal balances a HLT block attempt");
		vm_close(&blocked);
	}
	observer = open("/dev/" PVSCHED_HOOK_OBSERVER_DEVICE,
			O_RDWR | O_CLOEXEC);
	if (observer < 0) {
		result(0, "reopen observer for queued NMI fact");
	} else {
		ret = ioctl(vm.kvm, KVM_CHECK_EXTENSION, KVM_CAP_USER_NMI);
		if (ret < 0) {
			result(0, "query user NMI capability");
		} else if (!ret) {
			skip("queued NMI emits one interrupt fact",
			     "user NMI unavailable");
		} else {
			ret = ioctl(observer, PVSCHED_HOOK_IOC_RESET, 0);
			if (!ret)
				ret = ioctl(vm.vcpu, KVM_NMI, 0);
			result(!ret && !snapshot(observer, &s) && queued_nmi_fact(&s),
			       "queued NMI emits one interrupt fact");
		}
		close(observer);
	}
	vm_close(&vm);
	return failures || number != PLAN;
}
