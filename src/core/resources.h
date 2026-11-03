#ifndef PHI_RESOURCES_H
#define PHI_RESOURCES_H
/* Skills and prompt templates (Pi-compatible), from the user's config, the
   project, installed packages and resources_discover handlers.

   Skills: directories containing SKILL.md (Agent Skills format). The system
   prompt lists each skill's name, description and location; the model reads
   the file when a task matches. /skill:NAME [ARGS] loads one explicitly.

   Prompt templates: NAME.md files, used as /NAME [ARGS] with $1, $@,
   $ARGUMENTS, ${1:-default}, ${@:N}, ${@:N:L} substitution.

   Locations (user, then project, then packages):
     skills   $XDG_CONFIG_HOME/phi/skills, ~/.agents/skills,
              .phi/skills and .agents/skills from cwd up to the repository root
     prompts  $XDG_CONFIG_HOME/phi/prompts, .phi/prompts
   A name seen earlier wins; later duplicates are skipped. */
#include <stddef.h>
struct phi_command;

void resources_load(const char *reason); /* "startup" or "reload" */
void resources_close(void);
/* A template or skill command expanded to prompt text, or NULL when message
   is not one (built-in and extension commands take precedence). */
char *resources_expand(const char *message);
size_t resources_command_count(void);
const struct phi_command *resources_command_at(size_t index);
char *resources_system_prompt(void); /* skills section; caller frees */
/* One line per resource, for /skills and diagnostics; caller frees. */
char *resources_describe(void);
#endif
