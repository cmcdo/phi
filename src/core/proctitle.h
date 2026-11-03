#ifndef PHI_PROCTITLE_H
#define PHI_PROCTITLE_H
/* What ps and btop show for a Phi process, e.g. "phi chat d8aa2b58".
   Call proctitle_init first thing in main and use the argv it returns from
   then on: the title is written over the original argument strings. */
char **proctitle_init(int argc, char **argv);
/* name: the short process name (up to 15 bytes); title: the command line. */
void proctitle_set(const char *name, const char *title);
#endif
