/*
 * Copyright (c) 2008-2013 Apple Inc. All rights reserved.
 *
 * @APPLE_APACHE_LICENSE_HEADER_START@
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @APPLE_APACHE_LICENSE_HEADER_END@
 */

#include "internal.h"

// semaphores are too fundamental to use the dispatch_assume*() macros
#if USE_MACH_SEM
#define DISPATCH_SEMAPHORE_VERIFY_KR(x) do { \
		if (slowpath(x)) { \
			DISPATCH_CRASH("flawed group/semaphore logic"); \
		} \
	} while (0)
#elif USE_POSIX_SEM
#define DISPATCH_SEMAPHORE_VERIFY_RET(x) do { \
		if (slowpath((x) == -1)) { \
			DISPATCH_CRASH("flawed group/semaphore logic"); \
		} \
	} while (0)
#endif

#if USE_WIN32_SEM
// rdar://problem/8428132
static DWORD best_resolution = 1; // 1ms

DWORD
_push_timer_resolution(DWORD ms)
{
	MMRESULT res;
	static dispatch_once_t once;

	if (ms > 16) {
		// only update timer resolution if smaller than default 15.6ms
		// zero means not updated
		return 0;
	}

	// aim for the best resolution we can accomplish
	dispatch_once(&once, ^{
		TIMECAPS tc;
		MMRESULT res;
		res = timeGetDevCaps(&tc, sizeof(tc));
		if (res == MMSYSERR_NOERROR) {
			best_resolution = min(max(tc.wPeriodMin, best_resolution),
					tc.wPeriodMax);
		}
	});

	res = timeBeginPeriod(best_resolution);
	if (res == TIMERR_NOERROR) {
		return best_resolution;
	}
	// zero means not updated
	return 0;
}

// match ms parameter to result from _push_timer_resolution
void
_pop_timer_resolution(DWORD ms)
{
	if (ms) {
		timeEndPeriod(ms);
	}
}
#endif	/* USE_WIN32_SEM */


DISPATCH_WEAK // rdar://problem/8503746
long _dispatch_semaphore_signal_slow(dispatch_semaphore_t dsema);

static long _dispatch_group_wake(dispatch_semaphore_t dsema);

#pragma mark -
#pragma mark dispatch_semaphore_t

//初始化结构体信息
static void
_dispatch_semaphore_init(long value, dispatch_object_t dou)
{
	dispatch_semaphore_t dsema = dou._dsema;

	dsema->do_next = (dispatch_semaphore_t)DISPATCH_OBJECT_LISTLESS;
	dsema->do_targetq = dispatch_get_global_queue(
			DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
	//设置信号量的当前value值
	dsema->dsema_value = value;
	//设置信号量的初始value值
	dsema->dsema_orig = value;
#if USE_POSIX_SEM
	int ret = sem_init(&dsema->dsema_sem, 0, 0);
	DISPATCH_SEMAPHORE_VERIFY_RET(ret);
#endif
}

/*
 用来创建信号量，创建时需要指定value，内部会将value的值存储到dsema_value(当前的value)
 和dsema_orig(初始value)中，value的值必须大于或等于0。
 */
dispatch_semaphore_t
dispatch_semaphore_create(long value)
{
	dispatch_semaphore_t dsema;

	// If the internal value is negative, then the absolute of the value is
	// equal to the number of waiting threads. Therefore it is bogus to
	// initialize the semaphore with a negative value.
	if (value < 0) {
		//value值需大于或等于0
		return NULL;
	}
	//申请dispatch_semaphore_t的内存
	dsema = (dispatch_semaphore_t)_dispatch_alloc(DISPATCH_VTABLE(semaphore),
			sizeof(struct dispatch_semaphore_s) -
			sizeof(dsema->dsema_notify_head) -
			sizeof(dsema->dsema_notify_tail));
	//调用初始化函数
	_dispatch_semaphore_init(value, dsema);
	return dsema;
}

#if USE_MACH_SEM
static void
_dispatch_semaphore_create_port(semaphore_t *s4)
{
	kern_return_t kr;
	semaphore_t tmp;

	if (*s4) {
		return;
	}
	_dispatch_safe_fork = false;

	// lazily allocate the semaphore port

	// Someday:
	// 1) Switch to a doubly-linked FIFO in user-space.
	// 2) User-space timers for the timeout.
	// 3) Use the per-thread semaphore port.

	while ((kr = semaphore_create(mach_task_self(), &tmp,
			SYNC_POLICY_FIFO, 0))) {
		DISPATCH_VERIFY_MIG(kr);
		_dispatch_temporary_resource_shortage();
	}

	if (!dispatch_atomic_cmpxchg(s4, 0, tmp, relaxed)) {
		kr = semaphore_destroy(mach_task_self(), tmp);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
	}
}
#elif USE_WIN32_SEM
static void
_dispatch_semaphore_create_handle(HANDLE *s4)
{
	HANDLE tmp;

	if (*s4) {
		return;
	}

	// lazily allocate the semaphore port

	while (!dispatch_assume(tmp = CreateSemaphore(NULL, 0, LONG_MAX, NULL))) {
		_dispatch_temporary_resource_shortage();
	}

	if (!dispatch_atomic_cmpxchg(s4, 0, tmp)) {
		CloseHandle(tmp);
	}
}
#endif

void
_dispatch_semaphore_dispose(dispatch_object_t dou)
{
	dispatch_semaphore_t dsema = dou._dsema;

	if (dsema->dsema_value < dsema->dsema_orig) {
		//Warning:信号量还在使用的时候销毁会造成崩溃
		DISPATCH_CLIENT_CRASH(
				"Semaphore/group object deallocated while in use");
	}

#if USE_MACH_SEM
	kern_return_t kr;
	if (dsema->dsema_port) {
		kr = semaphore_destroy(mach_task_self(), dsema->dsema_port);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
	}
#elif USE_POSIX_SEM
	int ret = sem_destroy(&dsema->dsema_sem);
	DISPATCH_SEMAPHORE_VERIFY_RET(ret);
#elif USE_WIN32_SEM
	if (dsema->dsema_handle) {
		CloseHandle(dsema->dsema_handle);
	}
#endif
}

size_t
_dispatch_semaphore_debug(dispatch_object_t dou, char *buf, size_t bufsiz)
{
	dispatch_semaphore_t dsema = dou._dsema;

	size_t offset = 0;
	offset += dsnprintf(&buf[offset], bufsiz - offset, "%s[%p] = { ",
			dx_kind(dsema), dsema);
	offset += _dispatch_object_debug_attr(dsema, &buf[offset], bufsiz - offset);
#if USE_MACH_SEM
	offset += dsnprintf(&buf[offset], bufsiz - offset, "port = 0x%u, ",
			dsema->dsema_port);
#endif
	offset += dsnprintf(&buf[offset], bufsiz - offset,
			"value = %ld, orig = %ld }", dsema->dsema_value, dsema->dsema_orig);
	return offset;
}

//该函数会调用内核的semaphore_signal函数唤醒在dispatch_semaphore_wait中等待的线程
DISPATCH_NOINLINE
long
_dispatch_semaphore_signal_slow(dispatch_semaphore_t dsema)
{
	// Before dsema_sent_ksignals is incremented we can rely on the reference
	// held by the waiter. However, once this value is incremented the waiter
	// may return between the atomic increment and the semaphore_signal(),
	// therefore an explicit reference must be held in order to safely access
	// dsema after the atomic increment.
	_dispatch_retain(dsema);

#if USE_MACH_SEM || USE_POSIX_SEM
	(void)dispatch_atomic_inc2o(dsema, dsema_sent_ksignals, relaxed);
#endif

#if USE_MACH_SEM
	_dispatch_semaphore_create_port(&dsema->dsema_port);
	kern_return_t kr = semaphore_signal(dsema->dsema_port);
	DISPATCH_SEMAPHORE_VERIFY_KR(kr);
#elif USE_POSIX_SEM
	int ret = sem_post(&dsema->dsema_sem);
	DISPATCH_SEMAPHORE_VERIFY_RET(ret);
#elif USE_WIN32_SEM
	_dispatch_semaphore_create_handle(&dsema->dsema_handle);
	int ret = ReleaseSemaphore(dsema->dsema_handle, 1, NULL);
	dispatch_assume(ret);
#endif

	_dispatch_release(dsema);
	return 1;
}

long
dispatch_semaphore_signal(dispatch_semaphore_t dsema)
{
	// 首先将dsema_value调用原子方法加1，如果大于零就立即返回0
	long value = dispatch_atomic_inc2o(dsema, dsema_value, release);
	if (fastpath(value > 0)) {
		return 0;
	}
	if (slowpath(value == LONG_MIN)) {
		DISPATCH_CLIENT_CRASH("Unbalanced call to dispatch_semaphore_signal()");
	}
	return _dispatch_semaphore_signal_slow(dsema);
}

//等待信号量唤醒或者timeout超时
DISPATCH_NOINLINE
static long
_dispatch_semaphore_wait_slow(dispatch_semaphore_t dsema,
		dispatch_time_t timeout)
{
	long orig;

#if USE_MACH_SEM
	mach_timespec_t _timeout;
	kern_return_t kr;
#elif USE_POSIX_SEM
	struct timespec _timeout;
	int ret;
#elif USE_WIN32_SEM
	uint64_t nsec;
	DWORD msec;
	DWORD resolution;
	DWORD wait_result;
#endif

#if USE_MACH_SEM || USE_POSIX_SEM
again:
	// Mach semaphores appear to sometimes spuriously wake up. Therefore,
	// we keep a parallel count of the number of times a Mach semaphore is
	// signaled (6880961).
	orig = dsema->dsema_sent_ksignals;
	while (orig) {
		if (dispatch_atomic_cmpxchgvw2o(dsema, dsema_sent_ksignals, orig,
				orig - 1, &orig, relaxed)) {
			return 0;
		}
	}
#endif

#if USE_MACH_SEM
	_dispatch_semaphore_create_port(&dsema->dsema_port);
#elif USE_WIN32_SEM
	_dispatch_semaphore_create_handle(&dsema->dsema_handle);
#endif

	// From xnu/osfmk/kern/sync_sema.c:
	// wait_semaphore->count = -1; /* we don't keep an actual count */
	//
	// The code above does not match the documentation, and that fact is
	// not surprising. The documented semantics are clumsy to use in any
	// practical way. The above hack effectively tricks the rest of the
	// Mach semaphore logic to behave like the libdispatch algorithm.

	switch (timeout) {
	default:
#if USE_MACH_SEM
		do {
			//调用内核方法semaphore_timedwait计时等待，直到有信号到来或者超时了。
			uint64_t nsec = _dispatch_timeout(timeout);
			_timeout.tv_sec = (typeof(_timeout.tv_sec))(nsec / NSEC_PER_SEC);
			_timeout.tv_nsec = (typeof(_timeout.tv_nsec))(nsec % NSEC_PER_SEC);
			kr = slowpath(semaphore_timedwait(dsema->dsema_port, _timeout));
		} while (kr == KERN_ABORTED);

		if (kr != KERN_OPERATION_TIMED_OUT) {
			DISPATCH_SEMAPHORE_VERIFY_KR(kr);
			break;
		}
#elif USE_POSIX_SEM
		do {
			uint64_t nsec = _dispatch_timeout(timeout);
			_timeout.tv_sec = (typeof(_timeout.tv_sec))(nsec / NSEC_PER_SEC);
			_timeout.tv_nsec = (typeof(_timeout.tv_nsec))(nsec % NSEC_PER_SEC);
			ret = slowpath(sem_timedwait(&dsema->dsema_sem, &_timeout));
		} while (ret == -1 && errno == EINTR);

		if (ret == -1 && errno != ETIMEDOUT) {
			DISPATCH_SEMAPHORE_VERIFY_RET(ret);
			break;
		}
#elif USE_WIN32_SEM
		nsec = _dispatch_timeout(timeout);
		msec = (DWORD)(nsec / (uint64_t)1000000);
		resolution = _push_timer_resolution(msec);
		wait_result = WaitForSingleObject(dsema->dsema_handle, msec);
		_pop_timer_resolution(resolution);
		if (wait_result != WAIT_TIMEOUT) {
			break;
		}
#endif
		// Fall through and try to undo what the fast path did to
		// dsema->dsema_value
	case DISPATCH_TIME_NOW:
		orig = dsema->dsema_value;
		/*若desma_value小于0，对其加一并返回超时信号KERN_OPERATION_TIMED_OUT，
		 原子性加一是为了抵消dispatch_semaphore_wait函数开始的减一操作。*/
		while (orig < 0) {
			if (dispatch_atomic_cmpxchgvw2o(dsema, dsema_value, orig, orig + 1,
					&orig, relaxed)) {
#if USE_MACH_SEM
				return KERN_OPERATION_TIMED_OUT;
#elif USE_POSIX_SEM || USE_WIN32_SEM
				errno = ETIMEDOUT;
				return -1;
#endif
			}
		}
		// Another thread called semaphore_signal().
		// Fall through and drain the wakeup.
	case DISPATCH_TIME_FOREVER:
#if USE_MACH_SEM
		do {
			//调用系统的semaphore_wait方法，直到收到signal调用。
			kr = semaphore_wait(dsema->dsema_port);
		} while (kr == KERN_ABORTED);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
#elif USE_POSIX_SEM
		do {
			ret = sem_wait(&dsema->dsema_sem);
		} while (ret != 0);
		DISPATCH_SEMAPHORE_VERIFY_RET(ret);
#elif USE_WIN32_SEM
		WaitForSingleObject(dsema->dsema_handle, INFINITE);
#endif
		break;
	}
#if USE_MACH_SEM || USE_POSIX_SEM
	goto again;
#else
	return 0;
#endif
}

long
dispatch_semaphore_wait(dispatch_semaphore_t dsema, dispatch_time_t timeout)
{
	//先将信号量的dsema值原子性减一
	long value = dispatch_atomic_dec2o(dsema, dsema_value, acquire);
	//如果value大于等于0就立即返回
	if (fastpath(value >= 0)) {
		return 0;
	}
	return _dispatch_semaphore_wait_slow(dsema, timeout);
}

#pragma mark -
#pragma mark dispatch_group_t

// Dispatch Group的本质是一个初始value为LONG_MAX的semaphore，通过信号量来实现一组任务的管理
dispatch_group_t
dispatch_group_create(void)
{
	dispatch_group_t dg = (dispatch_group_t)_dispatch_alloc(
			DISPATCH_VTABLE(group), sizeof(struct dispatch_semaphore_s));
	_dispatch_semaphore_init(LONG_MAX, dg);
	return dg;
}

// dispatch_group_enter的逻辑是将dispatch_group_t转换成dispatch_semaphore_t后将dsema_value的值减一。
void
dispatch_group_enter(dispatch_group_t dg)
{
	dispatch_semaphore_t dsema = (dispatch_semaphore_t)dg;
	long value = dispatch_atomic_dec2o(dsema, dsema_value, acquire);
	if (slowpath(value < 0)) {
		DISPATCH_CLIENT_CRASH(
				"Too many nested calls to dispatch_group_enter()");
	}
}

/*
 dispatch_group_wake首先会循环调用semaphore_signal唤醒等待group的信号量，
 使dispatch_group_wait函数中等待的线程得以唤醒；然后依次获取链表中的元素并调
 用dispatch_async_f异步执行dispatch_group_notify函数中注册的回调，使得
 notify中的block得以执行。
 */
DISPATCH_NOINLINE
static long
_dispatch_group_wake(dispatch_semaphore_t dsema)
{
	dispatch_continuation_t next, head, tail = NULL, dc;
	long rval;
  	//将dsema的dsema_notify_head赋值为NULL，同时将之前的内容赋给head
	head = dispatch_atomic_xchg2o(dsema, dsema_notify_head, NULL, relaxed);
	if (head) {
		// snapshot before anything is notified/woken <rdar://problem/8554546>
		//将dsema的dsema_notify_tail赋值为NULL，同时将之前的内容赋给tail
		tail = dispatch_atomic_xchg2o(dsema, dsema_notify_tail, NULL, relaxed);
	}
	rval = (long)dispatch_atomic_xchg2o(dsema, dsema_group_waiters, 0, relaxed);
	if (rval) {
		// wake group waiters
#if USE_MACH_SEM
		_dispatch_semaphore_create_port(&dsema->dsema_port);
		do {
			kern_return_t kr = semaphore_signal(dsema->dsema_port);
			DISPATCH_SEMAPHORE_VERIFY_KR(kr);
		} while (--rval);
#elif USE_POSIX_SEM
		do {
			int ret = sem_post(&dsema->dsema_sem);
			DISPATCH_SEMAPHORE_VERIFY_RET(ret);
		} while (--rval);
#elif USE_WIN32_SEM
		_dispatch_semaphore_create_handle(&dsema->dsema_handle);
		int ret;
		ret = ReleaseSemaphore(dsema->dsema_handle, rval, NULL);
		dispatch_assume(ret);
#else
#error "No supported semaphore type"
#endif
	}
	if (head) {
		// async group notify blocks
		// 异步执行dispatch_group_notify函数中注册的回调
		do {
			next = fastpath(head->do_next);
			if (!next && head != tail) {
				while (!(next = fastpath(head->do_next))) {
					dispatch_hardware_pause();
				}
			}
			dispatch_queue_t dsn_queue = (dispatch_queue_t)head->dc_data;
			dc = _dispatch_continuation_free_cacheonly(head);
			dispatch_async_f(dsn_queue, head->dc_ctxt, head->dc_func);
			_dispatch_release(dsn_queue);
			if (slowpath(dc)) {
				_dispatch_continuation_free_to_cache_limit(dc);
			}
		} while ((head = next));
		_dispatch_release(dsema);
	}
	return 0;
}

//dispatch_group_leave的逻辑是将dispatch_group_t转换成dispatch_semaphore_t后将dsema_value的值加一。
void
dispatch_group_leave(dispatch_group_t dg)
{
	/*
	 当调用了dispatch_group_enter而没有调用dispatch_group_leave时，
	 会造成value值不等于LONG_MAX而不会走到唤醒逻辑，dispatch_group_notify
	 函数的block无法执行或者dispatch_group_wait收不到semaphore_signal
	 信号而卡住线程。当dispatch_group_leave比dispatch_group_enter
	 多调用了一次时，dispatch_semaphore_t的value会等于LONGMAX+1（2147483647+1）,
	 即long的负数最小值LONG_MIN(–2147483648)。因为此时value小于0，
	 所以会出现”Unbalanced call to dispatch_group_leave()”的崩溃，
	 这是一个特别需要注意的地方。
	 */
	dispatch_semaphore_t dsema = (dispatch_semaphore_t)dg;
	long value = dispatch_atomic_inc2o(dsema, dsema_value, release);
	if (slowpath(value < 0)) {
		DISPATCH_CLIENT_CRASH("Unbalanced call to dispatch_group_leave()");
	}
	if (slowpath(value == LONG_MAX)) {
		//当value等于LONG_MAX时表示所有任务已完成，调用_dispatch_group_wake唤醒group，因此dispatch_group_leave与dispatch_group_enter需成对出现。
		(void)_dispatch_group_wake(dsema);
	}
}

/*
 可以看到跟dispatch_semaphore的_dispatch_semaphore_wait_slow方法很类似，
 不同点在于等待完之后调用的again函数会调用_dispatch_group_wake唤醒当前group。
 _dispatch_group_wake的分析见下面的内容。
 */
DISPATCH_NOINLINE
static long
_dispatch_group_wait_slow(dispatch_semaphore_t dsema, dispatch_time_t timeout)
{
	long orig;

#if USE_MACH_SEM
	mach_timespec_t _timeout;
	kern_return_t kr;
#elif USE_POSIX_SEM // KVV
	struct timespec _timeout;
	int ret;
#elif USE_WIN32_SEM // KVV
	uint64_t nsec;
	DWORD msec;
	DWORD resolution;
	DWORD wait_result;
#endif

again:
	// check before we cause another signal to be sent by incrementing
	// dsema->dsema_group_waiters
	if (dsema->dsema_value == LONG_MAX) {
		return _dispatch_group_wake(dsema);
	}
	// Mach semaphores appear to sometimes spuriously wake up. Therefore,
	// we keep a parallel count of the number of times a Mach semaphore is
	// signaled (6880961).
	(void)dispatch_atomic_inc2o(dsema, dsema_group_waiters, relaxed);
	// check the values again in case we need to wake any threads
	if (dsema->dsema_value == LONG_MAX) {
		return _dispatch_group_wake(dsema);
	}

#if USE_MACH_SEM
	_dispatch_semaphore_create_port(&dsema->dsema_port);
#elif USE_WIN32_SEM
	_dispatch_semaphore_create_handle(&dsema->dsema_handle);
#endif

	// From xnu/osfmk/kern/sync_sema.c:
	// wait_semaphore->count = -1; /* we don't keep an actual count */
	//
	// The code above does not match the documentation, and that fact is
	// not surprising. The documented semantics are clumsy to use in any
	// practical way. The above hack effectively tricks the rest of the
	// Mach semaphore logic to behave like the libdispatch algorithm.

	switch (timeout) {
	default:
#if USE_MACH_SEM
		do {
			uint64_t nsec = _dispatch_timeout(timeout);
			_timeout.tv_sec = (typeof(_timeout.tv_sec))(nsec / NSEC_PER_SEC);
			_timeout.tv_nsec = (typeof(_timeout.tv_nsec))(nsec % NSEC_PER_SEC);
			kr = slowpath(semaphore_timedwait(dsema->dsema_port, _timeout));
		} while (kr == KERN_ABORTED);

		if (kr != KERN_OPERATION_TIMED_OUT) {
			DISPATCH_SEMAPHORE_VERIFY_KR(kr);
			break;
		}
#elif USE_POSIX_SEM
		do {
			uint64_t nsec = _dispatch_timeout(timeout);
			_timeout.tv_sec = (typeof(_timeout.tv_sec))(nsec / NSEC_PER_SEC);
			_timeout.tv_nsec = (typeof(_timeout.tv_nsec))(nsec % NSEC_PER_SEC);
			ret = slowpath(sem_timedwait(&dsema->dsema_sem, &_timeout));
		} while (ret == -1 && errno == EINTR);

		if (!(ret == -1 && errno == ETIMEDOUT)) {
			DISPATCH_SEMAPHORE_VERIFY_RET(ret);
			break;
		}
#elif USE_WIN32_SEM
		nsec = _dispatch_timeout(timeout);
		msec = (DWORD)(nsec / (uint64_t)1000000);
		resolution = _push_timer_resolution(msec);
		wait_result = WaitForSingleObject(dsema->dsema_handle, msec);
		_pop_timer_resolution(resolution);
		if (wait_result != WAIT_TIMEOUT) {
			break;
		}
#endif
		// Fall through and try to undo the earlier change to
		// dsema->dsema_group_waiters
	case DISPATCH_TIME_NOW:
		orig = dsema->dsema_group_waiters;
		while (orig) {
			if (dispatch_atomic_cmpxchgvw2o(dsema, dsema_group_waiters, orig,
					orig - 1, &orig, relaxed)) {
#if USE_MACH_SEM
				return KERN_OPERATION_TIMED_OUT;
#elif USE_POSIX_SEM || USE_WIN32_SEM
				errno = ETIMEDOUT;
				return -1;
#endif
			}
		}
		// Another thread called semaphore_signal().
		// Fall through and drain the wakeup.
	case DISPATCH_TIME_FOREVER:
#if USE_MACH_SEM
		do {
			kr = semaphore_wait(dsema->dsema_port);
		} while (kr == KERN_ABORTED);
		DISPATCH_SEMAPHORE_VERIFY_KR(kr);
#elif USE_POSIX_SEM
		do {
			ret = sem_wait(&dsema->dsema_sem);
		} while (ret == -1 && errno == EINTR);
		DISPATCH_SEMAPHORE_VERIFY_RET(ret);
#elif USE_WIN32_SEM
		WaitForSingleObject(dsema->dsema_handle, INFINITE);
#endif
		break;
	}
	goto again;
 }

// 如果当前value的值为初始值，表示任务都已经完成，直接返回0，如果timeout为0的话返回超时。其余情况会调用_dispatch_group_wait_slow方法。
long
dispatch_group_wait(dispatch_group_t dg, dispatch_time_t timeout)
{
	dispatch_semaphore_t dsema = (dispatch_semaphore_t)dg;

	if (dsema->dsema_value == LONG_MAX) {
		return 0;
	}
	if (timeout == 0) {
#if USE_MACH_SEM
		return KERN_OPERATION_TIMED_OUT;
#elif USE_POSIX_SEM || USE_WIN32_SEM
		errno = ETIMEDOUT;
		return (-1);
#endif
	}
	return _dispatch_group_wait_slow(dsema, timeout);
}

/*
 dispatch_group_notify的具体实现在dispatch_group_notify_f函数里，
 逻辑就是将block和queue封装到dispatch_continuation_t里，并将它加到链表的尾部，
 如果链表为空同时还会设置链表的头部节点。如果dsema_value的值等于初始值，
 则调用_dispatch_group_wake执行唤醒逻辑
 */
DISPATCH_NOINLINE
void
dispatch_group_notify_f(dispatch_group_t dg, dispatch_queue_t dq, void *ctxt,
		void (*func)(void *))
{
	dispatch_semaphore_t dsema = (dispatch_semaphore_t)dg;
	//封装结构体
	dispatch_continuation_t prev, dsn = _dispatch_continuation_alloc();
	dsn->do_vtable = (void *)DISPATCH_OBJ_ASYNC_BIT;
	dsn->dc_data = dq;
	dsn->dc_ctxt = ctxt;
	dsn->dc_func = func;
	dsn->do_next = NULL;
	_dispatch_retain(dq);
	//将结构体放到链表尾部，如果链表为空同时设置链表头部节点并唤醒group
	prev = dispatch_atomic_xchg2o(dsema, dsema_notify_tail, dsn, release);
	if (fastpath(prev)) {
		prev->do_next = dsn;
	} else {
		_dispatch_retain(dg);
		dispatch_atomic_store2o(dsema, dsema_notify_head, dsn, seq_cst);
		dispatch_atomic_barrier(seq_cst); // <rdar://problem/11750916>
		if (dispatch_atomic_load2o(dsema, dsema_value, seq_cst) == LONG_MAX) {
			_dispatch_group_wake(dsema);
		}
	}
}

#ifdef __BLOCKS__
void
dispatch_group_notify(dispatch_group_t dg, dispatch_queue_t dq,
		dispatch_block_t db)
{
	//封装调用dispatch_group_notify_f函数
	dispatch_group_notify_f(dg, dq, _dispatch_Block_copy(db),
			_dispatch_call_block_and_release);
}
#endif

#pragma mark -
#pragma mark _dispatch_thread_semaphore_t

_dispatch_thread_semaphore_t
_dispatch_thread_semaphore_create(void)
{
	_dispatch_safe_fork = false;
#if DISPATCH_USE_OS_SEMAPHORE_CACHE
	return _os_semaphore_create();
#elif USE_MACH_SEM
	semaphore_t s4;
	kern_return_t kr;
	while (slowpath(kr = semaphore_create(mach_task_self(), &s4,
			SYNC_POLICY_FIFO, 0))) {
		DISPATCH_VERIFY_MIG(kr);
		_dispatch_temporary_resource_shortage();
	}
	return s4;
#elif USE_POSIX_SEM
	sem_t s4;
	int ret = sem_init(&s4, 0, 0);
	DISPATCH_SEMAPHORE_VERIFY_RET(ret);
	return s4;
#elif USE_WIN32_SEM
	HANDLE tmp;
	while (!dispatch_assume(tmp = CreateSemaphore(NULL, 0, LONG_MAX, NULL))) {
		_dispatch_temporary_resource_shortage();
	}
	return (_dispatch_thread_semaphore_t)tmp;
#else
#error "No supported semaphore type"
#endif
}

void
_dispatch_thread_semaphore_dispose(_dispatch_thread_semaphore_t sema)
{
#if DISPATCH_USE_OS_SEMAPHORE_CACHE
	return _os_semaphore_dispose(sema);
#elif USE_MACH_SEM
	semaphore_t s4 = (semaphore_t)sema;
	kern_return_t kr = semaphore_destroy(mach_task_self(), s4);
	DISPATCH_SEMAPHORE_VERIFY_KR(kr);
#elif USE_POSIX_SEM
	sem_t s4 = (sem_t)sema;
	int ret = sem_destroy(&s4);
	DISPATCH_SEMAPHORE_VERIFY_RET(ret);
#elif USE_WIN32_SEM
	// XXX: signal the semaphore?
	WINBOOL success;
	success = CloseHandle((HANDLE)sema);
	dispatch_assume(success);
#else
#error "No supported semaphore type"
#endif
}

void
_dispatch_thread_semaphore_signal(_dispatch_thread_semaphore_t sema)
{
	// assumed to contain a release barrier
#if DISPATCH_USE_OS_SEMAPHORE_CACHE
	return _os_semaphore_signal(sema);
#elif USE_MACH_SEM
	semaphore_t s4 = (semaphore_t)sema;
	kern_return_t kr = semaphore_signal(s4);
	DISPATCH_SEMAPHORE_VERIFY_KR(kr);
#elif USE_POSIX_SEM
	sem_t s4 = (sem_t)sema;
	int ret = sem_post(&s4);
	DISPATCH_SEMAPHORE_VERIFY_RET(ret);
#elif USE_WIN32_SEM
	int ret;
	ret = ReleaseSemaphore((HANDLE)sema, 1, NULL);
	dispatch_assume(ret);
#else
#error "No supported semaphore type"
#endif
}

void
_dispatch_thread_semaphore_wait(_dispatch_thread_semaphore_t sema)
{
	// assumed to contain an acquire barrier
#if DISPATCH_USE_OS_SEMAPHORE_CACHE
	return _os_semaphore_wait(sema);
#elif USE_MACH_SEM
	semaphore_t s4 = (semaphore_t)sema;
	kern_return_t kr;
	do {
		kr = semaphore_wait(s4);
	} while (slowpath(kr == KERN_ABORTED));
	DISPATCH_SEMAPHORE_VERIFY_KR(kr);
#elif USE_POSIX_SEM
	sem_t s4 = (sem_t)sema;
	int ret;
	do {
		ret = sem_wait(&s4);
	} while (slowpath(ret != 0));
	DISPATCH_SEMAPHORE_VERIFY_RET(ret);
#elif USE_WIN32_SEM
	DWORD wait_result;
	do {
		wait_result = WaitForSingleObject((HANDLE)sema, INFINITE);
	} while (wait_result != WAIT_OBJECT_0);
#else
#error "No supported semaphore type"
#endif
}
