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
    s.add(ranged(keys::kWidth, 720, 480, 1400, "Pencere genişliği", "%100 ölçekte piksel"));
    s.add(ranged(keys::kMaxRows, 8, 3, 16, "Görünen satır sayısı"));
    s.add(ranged(keys::kFontSize, 14, 11, 22, "Yazı boyutu", "Sonuç başlıklarının boyutu (%100 ölçekte piksel); diğer metinler orantılı"));
    s.add(ranged(keys::kPosition, 22, 0, 70, "Dikey konum", "Pencerenin üst kenarı, ekran yüksekliğinin yüzdesi olarak"));
    s.add(def(keys::kAnimations, Kind::Bool, true, "Animasyonlar", "Açılışta kısa solma efekti"));
    s.add(def(keys::kFooter, Kind::Bool, true, "Alt bilgi çubuğu",
              "Pencerenin altında seçili öğenin bağlamı (git dalı, değişiklikler, yol) ve tuş ipuçları"));

    s.set_section_title("search", "Arama");
    s.add(ranged(keys::kMaxResults, 50, 10, 500, "En fazla sonuç", "Bir aramada sıralanıp listelenecek sonuç sayısı"));
    s.add(def(keys::kShowFrequent, Kind::Bool, true, "Boş aramada sık kullanılanlar",
              "Pencere boşken en sık/son kullandıklarını listele"));
    s.add(def(keys::kExcludeApps, Kind::GlobList, Value::List{"*uninstall*", "*kaldır*"}, "Gizlenecek uygulamalar",
              "Bu desenlere uyan uygulama adları listelenmez ('*' ve '?' kullanılabilir, büyük/küçük harf ve Türkçe karakter duyarsız)"));
    {
        Object code;
        code.fields = {{"alias", Value("code")}, {"target", Value("Visual Studio Code")}};
        auto d = def(keys::kAliases, Kind::ObjectList, Value::Objects{code}, "Takma adlar",
                     "Takma ad tam olarak yazılınca hedef uygulama en üste gelir");
        d.fields.push_back(field("alias", Kind::String, "", "not defteri", true));
        d.fields.push_back(field("target", Kind::String, "", "Notepad", true));
        s.add(std::move(d));
    }
    {
        auto folder = [](const char* path, int64_t priority, int64_t depth) {
            Object o;
            o.fields = {{"path", Value(path)}, {"priority", Value(priority)}, {"depth", Value(depth)}, {"include", Value(Value::List{})}};
            return o;
        };
        auto d = def(keys::kSearchFolders, Kind::ObjectList,
                     Value::Objects{folder("%USERPROFILE%\\Desktop", 20, 3), folder("%USERPROFILE%\\Documents", 0, 5)},
                     "Aranacak klasörler",
                     "Bu klasörlerdeki dosya ve klasörler (script'ler dahil) aramada çıkar ve 'priority' kadar öne alınır. "
                     "Script'lerde (.bat .cmd .ps1 .py .pyw .sh .exe) Enter çalıştırır, Alt+E düzenler");
        d.fields.push_back(field("path", Kind::Path, "", "'D:\\tools' (%ORTAM_DEĞİŞKENİ% kullanılabilir)", true));
        SettingDef prio = field("priority", Kind::Int, 40, "-100..100: öne alma puanı (tipik eşleşme puanı 50-300)");
        prio.min = -100;
        prio.max = 100;
        d.fields.push_back(std::move(prio));
        SettingDef depth = field("depth", Kind::Int, 6, "1..20: kaç klasör alta inilsin");
        depth.min = 1;
        depth.max = 20;
        d.fields.push_back(std::move(depth));
        d.fields.push_back(field("include", Kind::GlobList, Value::List{}, "sadece bu desenler, örn. ['*.py', '*.bat']; boş: tüm dosyalar"));
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    {
        auto d = def(keys::kExcludeDirs, Kind::GlobList,
                     Value::List{"node_modules", "__pycache__", "venv", "out", "build", "bin", "obj", "packages", "dist"},
                     "Aranmayacak klasörler", "Dosya indekslenirken içine girilmeyecek klasör adları (desen); '.' ile başlayanlar zaten atlanır");
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    {
        auto d = ranged(keys::kMaxFiles, 300000, 1000, 2000000, "En fazla dosya", "İndeksin üst sınırı (bellek: ~100 bayt/dosya)");
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    {
        auto d = def(keys::kFolderPriority, Kind::ObjectList, Value::Objects{}, "Klasör önceliği",
                     "Bu klasörlerin altındaki sonuçlar yukarı taşınır (eksi değer aşağı iter) ve bu klasörler de dosya aramasına "
                     "eklenir (derinlik 6). Yeni kurulumlarda search.folders kullanın; en uzun eşleşen klasör geçerlidir");
        d.fields.push_back(field("path", Kind::Path, "", "'D:\\src\\ana-proje' (%ORTAM_DEĞİŞKENİ% kullanılabilir)", true));
        SettingDef prio = field("priority", Kind::Int, 50, "-100..100 (varsayılan 50; tipik eşleşme puanı 50-300)");
        prio.min = -100;
        prio.max = 100;
        d.fields.push_back(std::move(prio));
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

    s.set_section_title("scripts", "Script'ler ve dosyalar");
    s.add(choice(keys::kScriptAction, {"run", "edit"}, "run", "Script'te Enter",
                 "run: çalıştırır (Alt+E düzenler) · edit: düzenler (Alt+R çalıştırır)"));
    s.add(choice(keys::kScriptEditor, {"auto", "code", "notepad", "default"}, "auto", "Düzenleyici",
                 "auto: VS Code varsa o, yoksa Not Defteri · default: dosya türünün 'Düzenle' komutu"));
    s.add(def(keys::kKeepConsole, Kind::Bool, true, "Konsol açık kalsın",
              ".bat/.cmd/.ps1/.py/.sh bitince pencere kapanmasın (çıktıyı görmek için)"));
    s.add(def(keys::kPython, Kind::Path, "", "Python yorumlayıcısı",
              "Boş: py launcher (py.exe / pyw.exe), yoksa PATH'teki python. Örnek: 'D:\\venvs\\tools\\Scripts\\python.exe'"));

    s.set_section_title("dev", "Geliştirme");
    s.add(choice(keys::kDefaultVs, {"vs2026", "vs2022"}, "vs2026", "Varsayılan Visual Studio",
                 "Build/debug komutlarının gönderileceği ve klasörlerin açılacağı sürüm (proje bazında değiştirilebilir)"));
    {
        auto d = def(keys::kProjectRoots, Kind::PathList, Value::List{"%USERPROFILE%\\source\\repos"}, "Proje kökleri",
                     "Git depoları bu klasörlerin altında aranır; %ORTAM_DEĞİŞKENİ% kullanılabilir. Örnek: ['D:\\src', 'D:\\work']");
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    {
        auto d = ranged(keys::kScanDepth, 4, 1, 8, "Tarama derinliği", "Proje köklerinin kaç klasör altına inilsin");
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    {
        auto d = def(keys::kScanExclude, Kind::GlobList,
                     Value::List{"node_modules", "out", "build", "bin", "obj", "__pycache__", "venv", "packages"},
                     "Taranmayacak klasörler", "Depo ararken içine girilmeyecek klasör adları (desen kullanılabilir)");
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    s.add(choice(keys::kRepoAction, {"vs", "code", "explorer", "terminal"}, "vs", "Depoda Enter",
                 "Bir git deposu seçilip Enter'a basılınca: Visual Studio (Open Folder), VS Code, Gezgin veya terminal"));
    s.add(choice(keys::kTerminal, {"wt", "cmd", "powershell", "git-bash"}, "wt", "Terminal",
                 "'Terminalde aç' eyleminin kullanacağı program (wt yoksa cmd)"));
    s.add(def(keys::kGitStatus, Kind::Bool, true, "Git durumunu göster",
              "Seçili deponun değişiklik sayısı ve ahead/behind bilgisi (git.exe çalıştırılır)"));
    s.add(def(keys::kBuildBeforeDebug, Kind::Bool, true, "Debug öncesi derle",
              "Debug komutu önce VS'te Build All çalıştırır, başarılıysa programı başlatıp debugger'ı bağlar"));
    s.add(def(keys::kDefaultArgs, Kind::String, "", "Varsayılan program argümanları",
              "Projede argüman girilmemişse kullanılır. {com}, {preset}, {target}, {config}, {project} yer tutucuları. Örnek: --port {com} --baud 115200"));
    s.add(def(keys::kVsConfigureCommand, Kind::String, "", "VS configure komutu",
              "Boş: VS komut tablosundan otomatik bulunur. Bulunamazsa 'Kamil: VS bağlantısını test et' raporundaki adı yazın"));
    s.add(def(keys::kVsReconfigureCommand, Kind::String, "", "VS önbelleği sil + yeniden yapılandır komutu",
              "Boş: otomatik bulunur (adında DeleteCache geçen komut)"));
    return s;
}

}  // namespace

const Schema& builtin_schema() {
    static const Schema schema = build();
    return schema;
}

}  // namespace kamil
