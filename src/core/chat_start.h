#ifndef PHI_CHAT_START_H
#define PHI_CHAT_START_H
#include <stddef.h>

/* Serialize hosted resume by transcript inode (aliases share a lock). The
   lock lives in the private runtime directory, never inside a transcript.
   A launcher closes its descriptor WITHOUT unlocking after starting a host:
   that host retains the shared lock until Phi publishes its live status.
   Failed children/hosts release their references on exit. */
int chat_start_lock(const char *directory, const char *file, char *error, size_t size);
/* In the forked launcher only: pass the descriptor through exec to phi-host. */
int chat_start_inherit(int fd);
/* At Phi entry: consume the inherited descriptor, make it close-on-exec so
   extensions/tools cannot retain it, and return it (-1 if none). */
int chat_start_receive(void);
/* After publishing status: unlock the shared description, then close it. */
void chat_start_release(int fd);
#endif
