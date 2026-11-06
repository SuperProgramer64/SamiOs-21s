/* SPDX-License-Identifier: MIT
 * Select a verified set of six virtual methods; never partially accept a build.
 * Concrete addresses belong to the caller's supported-build data.
 */
#ifndef HAKIOS_CA_PROFILE_H
#define HAKIOS_CA_PROFILE_H
struct hakios_ca_hook { uintptr_t slot,expected; };
struct hakios_ca_profile {
 const char *name;
 struct hakios_ca_hook hooks[6];
 unsigned update_flags;
 int legacy_gl;
 struct hakios_ca_hook gl_hooks[2];
};
static int
hakios_ca_hooks_match(const struct hakios_ca_hook *hooks,size_t count,
 int (*read_slot)(void *,uintptr_t,uintptr_t *),void *context)
{
 if(!count)return 0;
 for(size_t i=0;i<count;i++) {
  uintptr_t value;
  if(!hooks[i].expected || !read_slot(context,hooks[i].slot,&value) || value!=hooks[i].expected)return 0;
 }
 return 1;
}
static const struct hakios_ca_profile *
hakios_ca_choose(const struct hakios_ca_profile *profiles,size_t count,
 int (*read_slot)(void *,uintptr_t,uintptr_t *),void *context)
{
 for(size_t i=0;i<count;i++) {
  if(hakios_ca_hooks_match(profiles[i].hooks,6,read_slot,context))return profiles+i;
 }
 return NULL;
}
#endif
