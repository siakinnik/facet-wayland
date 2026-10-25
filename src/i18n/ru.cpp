// Russian translation.
#include "i18n/i18n.h"

namespace wayland {

namespace {
const facet::i18n::Table& ru() {
    static const facet::i18n::Table table = {
        {"Missing permissions.", "Нет нужных разрешений."},
        {"Showing the windows of {} apps", "Показывает окна приложений: {}"},
        {"Display server", "Дисплейный сервер"},
        {"State", "Состояние"},
        {"not running", "не запущен"},
        {"running", "работает"},
        {"Screen size", "Размер экрана"},
        {"Quality", "Качество"},
        {"App resolution", "Разрешение приложений"},
        {"Half (faster)", "Половинное (быстрее)"},
        {"Native", "Родное"},
        {"On a 4K screen apps draw four times as many pixels. Half keeps their layout and makes them much smoother; "
         "Native is sharper but slower.",
         "На экране 4K приложения рисуют в четыре раза больше пикселей. «Половинное» сохраняет раскладку и делает "
         "их заметно плавнее, «Родное» чётче, но медленнее."},
        {"Apps", "Приложения"},
        {"No app uses Wayland yet. Modules that need it (a browser, messengers) show their windows on their own screen.",
         "Пока ни одно приложение не использует Wayland. Модули, которым он нужен (браузер, мессенджеры), "
         "показывают свои окна на своём экране."},
        {"{} windows", "окон: {}"},
        {"Privacy", "Конфиденциальность"},
        {"Each app sees only its own windows. The clipboard and reading the screen need their own permissions in "
         "Settings > Apps.",
         "Каждое приложение видит только свои окна. Для буфера обмена и чтения экрана нужны отдельные разрешения "
         "в «Настройки > Приложения»."},
    };
    return table;
}
}  // namespace

void register_translations(facet::i18n::Catalog& catalog) { catalog.add("ru", ru()); }

}  // namespace wayland
