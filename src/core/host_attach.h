#ifndef PHI_HOST_ATTACH_H
#define PHI_HOST_ATTACH_H
/* Talking to phi-host (host/phi-host.c) over its socket. Frames are a type
   byte, a 16-bit length and data: 'i' input, 'w' window size, 'p' paste,
   'b' returning to a previously backgrounded chat. */
#include <stddef.h>

/* The host buffers a three-byte header plus this many payload bytes. */
#define PHI_HOST_FRAME_MAX 4093

/* Internal lifecycle notification, consumed before editor/modal key handling. */
#define BACKGROUNDED_NOTICE "\033[777;1~"
#define DETACH_REQUEST "\033]777;phi-detach\007"
/* Detach because the program is about to end (an empty chat closing). */
#define CLOSE_REQUEST "\033]777;phi-close\007"
#define RESET_MODES \
	"\033[<u\033[<u\033[<u\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1004l" \
	"\033[?2004l\033[?1l\033>\033[?25h\033[0m\033[?1049l"

int phi_host_write_all(int fd, const void *data, size_t size);
int phi_host_send_frame(int fd, char type, const void *data, size_t size);
int phi_host_connect_socket(const char *path);
/* Attach this terminal to the chat at path until it detaches or ends.
   Returns 0 when it ended, 3 when detached (still running), 4 when it
   detached because it is closing, 1 on error. name: print how to return. */
int phi_host_attach(const char *path, const char *name);
/* Phi chat revisit: notify its UI that first-background confirmation is done.
   Generic PROGRAM viewers use phi_host_attach without injecting this event. */
int phi_host_attach_backgrounded(const char *path, const char *name);
#endif
