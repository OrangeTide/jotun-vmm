/* task.h : preemptive tasks with spawn, exec, await, and notes.
 *
 * A task is a function on its own stack. A timer preempts running tasks, and
 * tasks also yield, exit, or return voluntarily. The scheduler is per-cpu over
 * a shared run queue, so ready tasks spread across whatever vcpus are idle.
 * One shared trust domain, no privilege split, so these are the process-model
 * primitives applied to tasks rather than isolated address spaces. */
#ifndef LIBOS_TASK_H
#define LIBOS_TASK_H

typedef void (*task_fn)(void);

/* The task-table size, so other subsystems can size per-task arrays. */
#define TASK_MAX    8

/* Bind this vcpu's per-cpu state and make arch_get_percpu return it. Each vcpu
 * calls this once, with its vcpu id, before touching any task primitive. */
void cpu_init(unsigned long id);

/* Longest exit-status string, including the terminator. Like a Plan 9 exit
 * status: empty means success, non-empty names the failure. */
#define STATUS_MAX  32

/* Note strings: a small per-task queue of posted messages (the cooperative
 * stand-in for signals until async delivery arrives with preemption). */
#define NOTE_MAX    48
#define NNOTES      4

/* Create a ready task that runs fn, as a child of the current task. name is
 * borrowed, not copied, so pass stable storage. Returns the task id, or -1 if
 * the table is full or a stack cannot be allocated. */
int task_spawn(const char *name, task_fn fn);

/* Replace the current task's program with fn, restarting on a fresh stack. The
 * task keeps its id, name, and parent. Does not return on success; returns -1
 * only if the stack cannot be reset. */
int task_exec(task_fn fn);

/* Run this cpu's scheduler until the system quiesces (no task running and the
 * run queue empty), then return to the caller. Every vcpu calls it; the boot
 * stack becomes that cpu's scheduler stack. An idle cpu blocks in the host
 * until another cpu queues work. */
void sched_start(void);

/* Yield the CPU to the next ready task. Returns when scheduled again. */
void task_yield(void);

/* Park the current task until task_wake(its id) is called. Unlike yield, a
 * blocked task is not runnable and is not rescheduled on its own. A wake that
 * races in just before the task parks is not lost. Used to wait on an external
 * event (for example network readiness) without busy-polling. */
void task_block(void);

/* Make a blocked task runnable again. Safe to call whether or not the task has
 * parked yet, and whether or not it is currently blocked. */
void task_wake(int id);

/* The id of the running task. Valid only inside a task, not the scheduler. */
int task_self(void);

/* Register a hook the scheduler calls on an event interrupt and whenever an
 * idle cpu is woken. A subsystem (net, draw) uses it to wake its own parked
 * tasks when a device event arrives. Several may be registered. */
void task_add_event_hook(void (*fn)(void));

/* End the current task with an exit status (NULL or "" means success). Does
 * not return. A task that returns from its function exits with success. */
void task_exits(const char *status) __attribute__((noreturn));

/* Wait for any child to exit. Copies its status into buf (truncated to nbuf)
 * and returns the child's id. Returns -1 at once if the caller has no
 * children. Yields while children are still running. */
int task_await(char *buf, int nbuf);

/* Post a note to a task. Borrowed string, copied into the target's queue.
 * Returns 0, or -1 if the id is invalid or its note queue is full. */
int task_note(int id, const char *note);

/* Receive the oldest note for the current task into buf. Returns 1 if a note
 * was delivered, 0 if the queue was empty. */
int task_recv_note(char *buf, int nbuf);

/* Print the task table: id, state, name. Observability for bring-up. */
void task_dump(void);

/* The event-interrupt handler, called from the arch ISR. Preempts the running
 * task by rescheduling. */
void irq_event(void);

#endif /* LIBOS_TASK_H */
