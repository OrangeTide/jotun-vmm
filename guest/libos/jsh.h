/* jsh.h : a small script-driven shell for the guest.
 *
 * There is no console input device yet, so jsh runs a script read from the
 * mounted bundle rather than a prompt. It is a libOS component, not a loaded
 * program, so it does not contend with command programs for the single load
 * region. */
#ifndef LIBOS_JSH_H
#define LIBOS_JSH_H

/* Read the script at the given namespace path and run each line. Requires the
 * 9P root to be mounted already. */
void jsh_run(const char *script_path);

#endif /* LIBOS_JSH_H */
