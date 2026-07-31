/*
 * port_env.h — registering accessor for boolean environment flags.
 *
 * Port/diagnostic behavior is gated on environment variables read at call sites
 * scattered across the tree. Read as bare getenv() string literals those gates
 * are undiscoverable and each site parses the value its own way.
 *
 * port_env_bool keeps the same seam (faithful decompiled code stays unmodified)
 * while making it uniform: the name is registered with its default and help text
 * the first time it is seen, the environment is read once per name, and the
 * parsed value is cached — so repeated calls are cheap and a flag cannot change
 * mid-run.
 *
 * Bool semantics:
 *   unset / empty  -> default_on
 *   "0"            -> off
 *   anything else  -> on
 * so `NAME=0` always means off, regardless of the default.
 *
 * A presence-only gate written as `getenv("X") != NULL` is NOT equivalent: it
 * treats "0" as on. Migrate such a site only when "0 means off" is intended.
 */
#ifndef MDKR_PORT_ENV_H
#define MDKR_PORT_ENV_H

#ifdef __cplusplus
extern "C" {
#endif

/* Registering, read-once boolean accessor. `help` is a short one-line
 * description recorded with the registration; pass NULL if genuinely none. */
int port_env_bool(const char *name, int default_on, const char *help);

#ifdef __cplusplus
}
#endif

#endif /* MDKR_PORT_ENV_H */
