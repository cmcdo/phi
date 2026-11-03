#ifndef PHI_BOARD_H
#define PHI_BOARD_H
#include <stddef.h>
/* The chat board (see board.c): returns CHATS_BOARD_RETURN, _OPEN (chosen
   holds the chat id) or _QUIT. */
int board_run(const char *current, char *chosen, size_t size);
#endif
