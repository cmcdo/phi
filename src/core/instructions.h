#ifndef PHI_INSTRUCTIONS_H
#define PHI_INSTRUCTIONS_H
char *instructions_load(void); /* caller frees */
void instructions_set_dynamic(char *(*provider)(void));
/* Skills section (resources_system_prompt); caller of provider frees. */
void instructions_set_skills(char *(*provider)(void));
/* Replace and/or extend the prompt for the current agent run; NULLs clear. */
void instructions_set_run(const char *replace, const char *append);
#endif
