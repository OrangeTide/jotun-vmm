/* task.h : cooperative round-robin tasks for the library OS.
 *
 * A task is a function on its own stack. Tasks run until they yield or return;
 * there is no preemption yet (that arrives with the timer in a later
 * milestone). One shared trust domain, no privilege split. */
#ifndef LIBOS_TASK_H
#define LIBOS_TASK_H

typedef void (*task_fn)(void);

/* Create a ready task that will run fn. name is borrowed, not copied, so pass
 * a string literal or other stable storage. Returns the task id, or -1 if the
 * table is full or a stack cannot be allocated. */
int task_spawn(const char *name, task_fn fn);

/* Run the scheduler until no task is ready, then return to the caller. Called
 * once from the boot context; the boot stack becomes the scheduler's stack. */
void sched_start(void);

/* Yield the CPU to the next ready task. Returns when this task is scheduled
 * again. */
void task_yield(void);

/* End the current task. Does not return. A task that returns from its function
 * exits through here automatically. */
void task_exit(void);

/* Print the task table: id, name, and state. Observability for bring-up. */
void task_dump(void);

#endif /* LIBOS_TASK_H */
