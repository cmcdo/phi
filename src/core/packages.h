#ifndef PHI_PACKAGES_H
#define PHI_PACKAGES_H
/* Phi packages: a git repository or local directory bundling C extensions,
   skills and prompt templates (Pi's package layout):

     extensions/NAME.c copied into the extensions directory; each still needs
                      approval by hash before it is compiled and loaded
     skills/          skill directories (SKILL.md)
     prompts/NAME.md   prompt templates

   package.json may list other locations under "phi" (or Pi's "pi") with
   "extensions", "skills" and "prompts" arrays of relative paths or globs.

   phi install git:github.com/user/repo[@ref] | https://…[@ref] | ./path
   phi remove NAME    phi list    phi update [NAME]

   Installed packages are recorded in $XDG_CONFIG_HOME/phi/packages.json;
   git packages are cloned under $XDG_DATA_HOME/phi/packages, local ones are
   used in place. */
#include <json-c/json.h>

json_object *packages_installed(void); /* [{name, source, path, ref, extensions}] */
json_object *packages_manifest(const char *root); /* the "phi"/"pi" object, or NULL */
/* Handle a package subcommand. Returns an exit status, or -1 when argv is
   not a package command. */
int packages_cli(int argc, char **argv);
#endif
