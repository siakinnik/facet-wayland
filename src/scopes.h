// Which Wayland globals a client may bind, by the wayland.* scopes the user
// granted its module (see facet-core docs, "Wayland scopes are per client").
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The scope a global needs: NULL = every client may bind it, "" = no client
// ever (compositor-private protocols such as virtual input or layer shell).
const char* fw_scope_for_global(const char* interface_name);

// True if a client whose module holds `scopes` may bind the global.
bool fw_scope_allows(const char* interface_name, const char* const* scopes, int n_scopes);

#ifdef __cplusplus
}
#endif
