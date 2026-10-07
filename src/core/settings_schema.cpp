#include "core/settings_schema.h"

namespace kamil {

namespace {

SettingDef def(std::string key, Kind kind, Value value, std::string title, std::string description = {}) {
    SettingDef d;
    d.key = std::move(key);
    d.kind = kind;
    d.def = std::move(value);
    d.title = std::move(title);
    d.description = std::move(description);
    return d;
}

SettingDef ranged(std::string key, int64_t value, int64_t min, int64_t max, std::string title, std::string description = {}) {
    SettingDef d = def(std::move(key), Kind::Int, Value(value), std::move(title), std::move(description));
    d.min = min;
    d.max = max;
    return d;
}

SettingDef choice(std::string key, std::vector<std::string> choices, std::string value, std::string title,
                  std::string description = {}) {
    SettingDef d = def(std::move(key), Kind::Enum, Value(std::move(value)), std::move(title), std::move(description));
    d.choices = std::move(choices);
    return d;
}

SettingDef field(std::string name, Kind kind, Value value, std::string description, bool required = false) {
    SettingDef d = def(std::move(name), kind, std::move(value), {}, std::move(description));
    d.required = required;
    return d;
}

Schema build() {
    Schema s;

    s.set_section_title("general", "Genel");
    {
        auto d = def(keys::kHotkey, Kind::Hotkey, "Alt+Space", "Ana kısayol",
                     "Kamil'i açıp kapatan global kısayol. Örnek: Alt+Space, Ctrl+Space, Win+Alt+K");
        d.apply = Apply::Hotkey;
        s.add(std::move(d));
    }
    s.add(def(keys::kStartWithWindows, Kind::Bool, false, "Windows ile başlat",
              "Oturum açılınca Kamil arka planda başlasın"));
    s.add(def(keys::kRememberQuery, Kind::Bool, false, "Son aramayı hatırla",
              "Pencere yeniden açıldığında son yazılan metin seçili olarak gelsin"));
    s.add(def(keys::kHideOnFocusLoss, Kind::Bool, true, "Odak kaybolunca gizle",
              "Başka bir pencereye tıklanınca Kamil kapansın"));

    s.set_section_title("appearance", "Görünüm");
    s.add(choice(keys::kTheme, {"auto", "dark", "light"}, "auto", "Tema", "auto: Windows uygulama temasını izler"));
    s.add(def(keys::kAccent, Kind::Color, "auto", "Vurgu rengi",
              "auto: Windows vurgu rengi; ya da #RRGGBB (seçim ve eşleşen harfler)"));
    s.add(ranged(keys::kWidth, 720, 480, 1400, "Pencere genişliği", "Piksel (100% ölçekte)"));
    s.add(ranged(keys::kMaxRows, 8, 3, 16, "Görünen satır sayısı"));
    s.add(ranged(keys::kFontSize, 14, 11, 22, "Yazı boyutu", "Sonuç başlıklarının punto değeri; diğer metinler orantılı"));
    s.add(ranged(keys::kPosition, 22, 0, 70, "Dikey konum", "Pencerenin üst kenarı, ekran yüksekliğinin yüzdesi olarak"));
    s.add(def(keys::kAnimations, Kind::Bool, true, "Animasyonlar", "Açılışta kısa solma efekti"));

    s.set_section_title("search", "Arama");
    s.add(ranged(keys::kMaxResults, 50, 10, 500, "En fazla sonuç", "Bir aramada sıralanıp listelenecek sonuç sayısı"));
    s.add(def(keys::kShowFrequent, Kind::Bool, true, "Boş aramada sık kullanılanlar",
              "Pencere boşken en sık/son kullandıklarını listele"));
    s.add(def(keys::kExcludeApps, Kind::GlobList, Value::List{"*uninstall*", "*kaldır*"}, "Gizlenecek uygulamalar",
              "Bu desenlere uyan uygulama adları listelenmez ('*' ve '?' kullanılabilir, büyük/küçük harf ve Türkçe karakter duyarsız)"));
    {
        auto d = def(keys::kAliases, Kind::ObjectList, Value::Objects{}, "Takma adlar",
                     "Takma ad tam olarak yazılınca hedef uygulama en üste gelir");
        d.fields.push_back(field("alias", Kind::String, "", "not defteri", true));
        d.fields.push_back(field("target", Kind::String, "", "Notepad", true));
        s.add(std::move(d));
    }
    s.add(def(keys::kLearning, Kind::Bool, true, "Seçimlerden öğren",
              "Hangi aramada neyi seçtiğini hatırlayıp sıralamayı buna göre düzenler (veriler sadece bu bilgisayarda)"));
    {
        auto d = def(keys::kLearningHalfLife, Kind::Duration, Value(int64_t{14} * 86'400'000), "Öğrenme yarı ömrü",
                     "Bir seçimin etkisinin yarıya inme süresi");
        d.min = 86'400'000;
        d.max = int64_t{365} * 86'400'000;
        s.add(std::move(d));
    }

    s.set_section_title("dev", "Geliştirme");
    s.add(choice(keys::kDefaultVs, {"vs2026", "vs2022"}, "vs2026", "Varsayılan Visual Studio",
                 "Build/debug komutlarının gönderileceği sürüm (proje bazında değiştirilebilir)"));
    return s;
}

}  // namespace

const Schema& builtin_schema() {
    static const Schema schema = build();
    return schema;
}

}  // namespace kamil
