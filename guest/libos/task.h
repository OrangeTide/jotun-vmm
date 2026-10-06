/* task.h : cooperative tasks with spawn, exec, await, and notes.
 *
 * A task is a function on its own stack. Tasks run until they yield, exit, or
 * return; there is no preemption yet (that arrives with the timer in a later
 * milestone). One shared trust domain, no privilege split, so these are the
 * process-model primitives applied to tasks rather than isolated address
 * spaces. A loader turns a name into a function at M3; for now callers pass a
 * function directly. */
#ifndef LIBOS_TASK_H
#define LIBOS_TASK_H

typedef void (*task_fn)(void);

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

/* Run the scheduler until no task is ready, then return to the caller. Called
 * once from the boot context; the boot stack becomes the scheduler's stack. */
void sched_start(void);

/* Yield the CPU to the next ready task. Returns when scheduled again. */
void task_yield(void);

/* End the current task with an exit status (NULL or "" means success). Does
 * not return. A task that returns from its function exits with success. */
void task_exits(const char *status);

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

#endif /* LIBOS_TASK_H */
