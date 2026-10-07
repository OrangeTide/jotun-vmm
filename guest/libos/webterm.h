/* webterm.h : a jsh terminal rendered over the draw channel.
 *
 * A monospace text screen shown through the draw pixel-free vector path, driven
 * by draw input events: keystrokes (DEV_TEXT) and paste (DEV_PASTE) edit a
 * command line, Enter runs it through jsh, and console output is mirrored to the
 * screen by a registered console sink. The usable, keyboard-driven counterpart
 * to the graphical draw demos. */
#ifndef LIBOS_WEBTERM_H
#define LIBOS_WEBTERM_H

/* Run the web terminal. Dispatched by the "webterm" boot argument. Returns when
 * the client closes the window. */
void webterm(void);

#endif /* LIBOS_WEBTERM_H */
