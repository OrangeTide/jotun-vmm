/* p9mem.h : the in-memory 9P backend used to bring up the 9P path. */
#ifndef P9MEM_H
#define P9MEM_H

#include "p9srv.h"

const struct p9_backend *p9mem_backend(void);

#endif /* P9MEM_H */
