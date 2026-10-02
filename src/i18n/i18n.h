// Translations of the plugin's UI. Strings in the code are English and act as
// keys; each language is a table in this directory.
#pragma once

#include "facet/i18n.h"

namespace wayland {

void register_translations(facet::i18n::Catalog& catalog);

}  // namespace wayland
