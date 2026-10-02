// Russian translation of the example.
#include "i18n/i18n.h"

namespace desktop_app {

namespace {
const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        {"The app has closed.", "Приложение закрылось."},
        {"Waiting for the Wayland module…", "Жду модуль Wayland…"},
        {"No command configured.", "Команда не задана."},
        {"Starting…", "Запуск…"},
        {"Could not start the app.", "Не удалось запустить приложение."},
        {"running", "работает"},
        {"stopped", "остановлено"},
        {"Desktop app", "Приложение для компьютера"},
        {"Command", "Команда"},
        {"State", "Состояние"},
        {"Start", "Запустить"},
    };
    return table;
}
}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace desktop_app
