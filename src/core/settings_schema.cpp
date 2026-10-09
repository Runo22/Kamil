#include "core/settings_schema.h"

namespace kamil {

namespace {

// A user visible text in both languages.
struct T {
    const char* en;
    const char* tr;
};

SettingDef def(std::string key, Kind kind, Value value, T title, T description = {"", ""}) {
    SettingDef d;
    d.key = std::move(key);
    d.kind = kind;
    d.def = std::move(value);
    d.title = title.en;
    d.title_tr = title.tr;
    d.description = description.en;
    d.description_tr = description.tr;
    return d;
}

SettingDef ranged(std::string key, int64_t value, int64_t min, int64_t max, T title, T description = {"", ""}) {
    SettingDef d = def(std::move(key), Kind::Int, Value(value), title, description);
    d.min = min;
    d.max = max;
    return d;
}

SettingDef choice(std::string key, std::vector<std::string> choices, std::string value, T title, T description = {"", ""}) {
    SettingDef d = def(std::move(key), Kind::Enum, Value(std::move(value)), title, description);
    d.choices = std::move(choices);
    return d;
}

SettingDef field(std::string name, Kind kind, Value value, T description, bool required = false) {
    SettingDef d = def(std::move(name), kind, std::move(value), {"", ""}, description);
    d.required = required;
    return d;
}

Schema build() {
    Schema s;

    s.set_section_title("general", "General", "Genel");
    s.add(choice(keys::kLanguage, {"auto", "en", "tr"}, "auto", {"Language", "Dil"},
                 {"auto: follows the Windows display language", "auto: Windows görüntü dilini izler"}));
    {
        auto d = def(keys::kHotkey, Kind::Hotkey, "Alt+Space", {"Hotkey", "Ana kısayol"},
                     {"Global hotkey that shows and hides Kamil. Examples: Alt+Space, Ctrl+Space, Win+Alt+K",
                      "Kamil'i açıp kapatan global kısayol. Örnek: Alt+Space, Ctrl+Space, Win+Alt+K"});
        d.apply = Apply::Hotkey;
        s.add(std::move(d));
    }
    s.add(def(keys::kStartWithWindows, Kind::Bool, false, {"Start with Windows", "Windows ile başlat"},
              {"Start Kamil in the background when you sign in", "Oturum açılınca Kamil arka planda başlasın"}));
    s.add(def(keys::kRememberQuery, Kind::Bool, false, {"Remember last search", "Son aramayı hatırla"},
              {"Show the last search (selected) when the window opens again", "Pencere yeniden açıldığında son yazılan metin seçili olarak gelsin"}));
    s.add(def(keys::kHideOnFocusLoss, Kind::Bool, true, {"Hide on focus loss", "Odak kaybolunca gizle"},
              {"Close Kamil when another window is clicked", "Başka bir pencereye tıklanınca Kamil kapansın"}));

    s.set_section_title("appearance", "Appearance", "Görünüm");
    s.add(choice(keys::kTheme, {"auto", "dark", "light"}, "auto", {"Theme", "Tema"},
                 {"auto: follows the Windows app theme", "auto: Windows uygulama temasını izler"}));
    s.add(def(keys::kAccent, Kind::Color, "auto", {"Accent color", "Vurgu rengi"},
              {"auto: Windows accent color, or #RRGGBB (selection and matched letters)",
               "auto: Windows vurgu rengi; ya da #RRGGBB (seçim ve eşleşen harfler)"}));
    s.add(ranged(keys::kWidth, 720, 480, 1400, {"Window width", "Pencere genişliği"}, {"Pixels at 100% scale", "%100 ölçekte piksel"}));
    s.add(ranged(keys::kMaxRows, 8, 3, 16, {"Visible rows", "Görünen satır sayısı"}));
    s.add(ranged(keys::kFontSize, 14, 11, 22, {"Font size", "Yazı boyutu"},
                 {"Size of result titles (pixels at 100%); other text scales with it",
                  "Sonuç başlıklarının boyutu (%100 ölçekte piksel); diğer metinler orantılı"}));
    s.add(ranged(keys::kPosition, 22, 0, 70, {"Vertical position", "Dikey konum"},
                 {"Top edge of the window, in percent of the screen height", "Pencerenin üst kenarı, ekran yüksekliğinin yüzdesi olarak"}));
    s.add(def(keys::kAnimations, Kind::Bool, true, {"Animations", "Animasyonlar"}, {"Short fade when opening", "Açılışta kısa solma efekti"}));
    s.add(def(keys::kFooter, Kind::Bool, true, {"Footer bar", "Alt bilgi çubuğu"},
              {"Context of the selected item (git branch, changes, path, running job) and key hints",
               "Seçili öğenin bağlamı (git dalı, değişiklikler, yol, çalışan iş) ve tuş ipuçları"}));

    s.set_section_title("search", "Search", "Arama");
    s.add(ranged(keys::kMaxResults, 50, 10, 500, {"Maximum results", "En fazla sonuç"},
                 {"Number of results ranked and listed per search", "Bir aramada sıralanıp listelenecek sonuç sayısı"}));
    s.add(def(keys::kShowFrequent, Kind::Bool, true, {"Frequent items when empty", "Boş aramada sık kullanılanlar"},
              {"List the most frequently / recently used items when the search box is empty",
               "Pencere boşken en sık/son kullandıklarını listele"}));
    s.add(def(keys::kExcludeApps, Kind::GlobList, Value::List{"*uninstall*", "*kaldır*"}, {"Hidden apps", "Gizlenecek uygulamalar"},
              {"Apps whose names match these patterns are not listed ('*' and '?'; case and accent insensitive)",
               "Bu desenlere uyan uygulama adları listelenmez ('*' ve '?' kullanılabilir, büyük/küçük harf ve Türkçe karakter duyarsız)"}));
    {
        Object code;
        code.fields = {{"alias", Value("code")}, {"target", Value("Visual Studio Code")}};
        auto d = def(keys::kAliases, Kind::ObjectList, Value::Objects{code}, {"Aliases", "Takma adlar"},
                     {"Typing an alias exactly puts its target app first", "Takma ad tam olarak yazılınca hedef uygulama en üste gelir"});
        d.fields.push_back(field("alias", Kind::String, "", {"notepad", "not defteri"}, true));
        d.fields.push_back(field("target", Kind::String, "", {"Notepad", "Notepad"}, true));
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
                     {"Search folders", "Aranacak klasörler"},
                     {"Files and folders here (scripts included) appear in search, moved up by 'priority'. "
                      "Scripts (.bat .cmd .ps1 .py .pyw .sh .exe): Enter runs, Alt+E edits",
                      "Bu klasörlerdeki dosya ve klasörler (script'ler dahil) aramada çıkar ve 'priority' kadar öne alınır. "
                      "Script'lerde (.bat .cmd .ps1 .py .pyw .sh .exe) Enter çalıştırır, Alt+E düzenler"});
        d.fields.push_back(field("path", Kind::Path, "", {"'D:\\tools' (%ENVIRONMENT_VARIABLES% allowed)", "'D:\\tools' (%ORTAM_DEĞİŞKENİ% kullanılabilir)"}, true));
        SettingDef prio = field("priority", Kind::Int, 40, {"-100..100: boost (typical match scores are 50-300)", "-100..100: öne alma puanı (tipik eşleşme puanı 50-300)"});
        prio.min = -100;
        prio.max = 100;
        d.fields.push_back(std::move(prio));
        SettingDef depth = field("depth", Kind::Int, 6, {"1..20: how many folder levels down", "1..20: kaç klasör alta inilsin"});
        depth.min = 1;
        depth.max = 20;
        d.fields.push_back(std::move(depth));
        d.fields.push_back(field("include", Kind::GlobList, Value::List{},
                                 {"only these patterns, e.g. ['*.py', '*.bat']; empty: all files",
                                  "sadece bu desenler, örn. ['*.py', '*.bat']; boş: tüm dosyalar"}));
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    {
        auto d = def(keys::kExcludeDirs, Kind::GlobList,
                     Value::List{"node_modules", "__pycache__", "venv", "out", "build", "bin", "obj", "packages", "dist"},
                     {"Skipped folders", "Aranmayacak klasörler"},
                     {"Folder names (patterns) not entered while indexing; names starting with '.' are always skipped",
                      "Dosya indekslenirken içine girilmeyecek klasör adları (desen); '.' ile başlayanlar zaten atlanır"});
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    {
        auto d = ranged(keys::kMaxFiles, 300000, 1000, 2000000, {"Maximum files", "En fazla dosya"},
                        {"Upper limit of the index (memory: ~100 bytes per file)", "İndeksin üst sınırı (bellek: ~100 bayt/dosya)"});
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    {
        auto d = def(keys::kFolderPriority, Kind::ObjectList, Value::Objects{}, {"Folder priority", "Klasör önceliği"},
                     {"Results under these folders move up (a negative value pushes them down) and the folders are searched too "
                      "(depth 6). Prefer search.folders; the longest matching folder wins",
                      "Bu klasörlerin altındaki sonuçlar yukarı taşınır (eksi değer aşağı iter) ve bu klasörler de dosya aramasına "
                      "eklenir (derinlik 6). Yeni kurulumlarda search.folders kullanın; en uzun eşleşen klasör geçerlidir"});
        d.fields.push_back(field("path", Kind::Path, "", {"'D:\\src\\main-project'", "'D:\\src\\ana-proje'"}, true));
        SettingDef prio = field("priority", Kind::Int, 50, {"-100..100 (default 50)", "-100..100 (varsayılan 50)"});
        prio.min = -100;
        prio.max = 100;
        d.fields.push_back(std::move(prio));
        s.add(std::move(d));
    }
    s.add(def(keys::kLearning, Kind::Bool, true, {"Learn from choices", "Seçimlerden öğren"},
              {"Remembers what you pick for which search and ranks accordingly (data stays on this computer)",
               "Hangi aramada neyi seçtiğini hatırlayıp sıralamayı buna göre düzenler (veriler sadece bu bilgisayarda)"}));
    {
        auto d = def(keys::kLearningHalfLife, Kind::Duration, Value(int64_t{14} * 86'400'000), {"Learning half-life", "Öğrenme yarı ömrü"},
                     {"Time after which a choice counts half as much", "Bir seçimin etkisinin yarıya inme süresi"});
        d.min = 86'400'000;
        d.max = int64_t{365} * 86'400'000;
        s.add(std::move(d));
    }

    s.set_section_title("scripts", "Scripts and files", "Script'ler ve dosyalar");
    s.add(choice(keys::kScriptAction, {"run", "edit"}, "run", {"Enter on a script", "Script'te Enter"},
                 {"run: runs it (Alt+E edits) · edit: edits it (Alt+R runs)", "run: çalıştırır (Alt+E düzenler) · edit: düzenler (Alt+R çalıştırır)"}));
    s.add(choice(keys::kScriptEditor, {"auto", "code", "notepad", "default"}, "auto", {"Editor", "Düzenleyici"},
                 {"auto: VS Code if installed, otherwise Notepad · default: the file type's 'Edit' command",
                  "auto: VS Code varsa o, yoksa Not Defteri · default: dosya türünün 'Düzenle' komutu"}));
    s.add(def(keys::kKeepConsole, Kind::Bool, true, {"Keep console open", "Konsol açık kalsın"},
              {"Keep the window of .bat/.cmd/.ps1/.py/.sh open after the script ends (to read its output)",
               ".bat/.cmd/.ps1/.py/.sh bitince pencere kapanmasın (çıktıyı görmek için)"}));
    s.add(def(keys::kPython, Kind::Path, "", {"Python interpreter", "Python yorumlayıcısı"},
              {"Empty: py launcher (py.exe / pyw.exe), else python on PATH. Example: 'D:\\venvs\\tools\\Scripts\\python.exe'",
               "Boş: py launcher (py.exe / pyw.exe), yoksa PATH'teki python. Örnek: 'D:\\venvs\\tools\\Scripts\\python.exe'"}));

    s.set_section_title("dev", "Development", "Geliştirme");
    s.add(choice(keys::kDefaultVs, {"vs2026", "vs2022"}, "vs2026", {"Default Visual Studio", "Varsayılan Visual Studio"},
                 {"Version that receives build/debug commands and opens folders",
                  "Build/debug komutlarının gönderileceği ve klasörlerin açılacağı sürüm"}));
    s.add(def(keys::kDevenvPath, Kind::Path, "", {"devenv.exe path", "devenv.exe yolu"},
              {"Empty: found automatically (vswhere, then the standard install folders). Set it if Visual Studio is installed somewhere unusual",
               "Boş: otomatik bulunur (vswhere, sonra standart kurulum klasörleri). Visual Studio alışılmadık bir yerdeyse yolu verin"}));
    {
        auto d = def(keys::kProjectRoots, Kind::PathList, Value::List{"%USERPROFILE%\\source\\repos"}, {"Project roots", "Proje kökleri"},
                     {"Git repositories and CMake projects are searched below these folders. Example: ['D:\\src', 'D:\\work']",
                      "Git depoları ve CMake projeleri bu klasörlerin altında aranır. Örnek: ['D:\\src', 'D:\\work']"});
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    {
        auto d = ranged(keys::kScanDepth, 4, 1, 8, {"Scan depth", "Tarama derinliği"},
                        {"How many folder levels below the project roots", "Proje köklerinin kaç klasör altına inilsin"});
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    {
        auto d = def(keys::kScanExclude, Kind::GlobList, Value::List{"node_modules", "out", "build", "bin", "obj", "__pycache__", "venv", "packages"},
                     {"Skipped folders", "Taranmayacak klasörler"},
                     {"Folder names (patterns) not entered while looking for repositories", "Depo ararken içine girilmeyecek klasör adları (desen)"});
        d.apply = Apply::Reindex;
        s.add(std::move(d));
    }
    s.add(choice(keys::kRepoAction, {"vs", "code", "explorer", "terminal"}, "vs", {"Enter on a repository", "Depoda Enter"},
                 {"Visual Studio (Open Folder), VS Code, Explorer or a terminal", "Visual Studio (Open Folder), VS Code, Gezgin veya terminal"}));
    s.add(choice(keys::kTerminal, {"wt", "cmd", "powershell", "git-bash"}, "wt", {"Terminal", "Terminal"},
                 {"Program used by 'Open in terminal' (cmd if Windows Terminal is missing)", "'Terminalde aç' eyleminin kullanacağı program (wt yoksa cmd)"}));
    s.add(def(keys::kGitStatus, Kind::Bool, true, {"Show git status", "Git durumunu göster"},
              {"Changes and ahead/behind of the selected repository (runs git.exe)", "Seçili deponun değişiklik sayısı ve ahead/behind bilgisi (git.exe çalıştırılır)"}));
    s.add(def(keys::kBuildBeforeDebug, Kind::Bool, true, {"Build before debug", "Debug öncesi derle"},
              {"Debug first runs Build All in VS; on success it starts the program and attaches the debugger",
               "Debug komutu önce VS'te Build All çalıştırır, başarılıysa programı başlatıp debugger'ı bağlar"}));
    s.add(def(keys::kDefaultArgs, Kind::String, "", {"Default program arguments", "Varsayılan program argümanları"},
              {"Used when a project has no arguments of its own. Placeholders {com} {preset} {target} {config} {project}. Example: --port {com} --baud 115200",
               "Projede argüman girilmemişse kullanılır. {com} {preset} {target} {config} {project} yer tutucuları. Örnek: --port {com} --baud 115200"}));
    s.add(def(keys::kVsConfigureCommand, Kind::String, "", {"VS configure command", "VS configure komutu"},
              {"Empty: found in the VS command table. If not found, copy the name from the 'Kamil: Test Visual Studio connection' report",
               "Boş: VS komut tablosundan otomatik bulunur. Bulunamazsa 'Kamil: VS bağlantısını test et' raporundaki adı yazın"}));
    s.add(def(keys::kVsReconfigureCommand, Kind::String, "", {"VS delete cache + reconfigure command", "VS önbelleği sil + yeniden yapılandır komutu"},
              {"Empty: found automatically (a command containing DeleteCache)", "Boş: otomatik bulunur (adında DeleteCache geçen komut)"}));
    {
        auto d = ranged(keys::kVsWaitSeconds, 90, 5, 600, {"VS wait limit", "VS bekleme sınırı"},
                        {"Seconds Kamil waits for Visual Studio to start, or to enable a command (e.g. while CMake is generating), before giving up. Waiting jobs can be cancelled from 'Kamil: VS jobs'",
                         "Visual Studio açılana ya da bir komutu etkinleştirene kadar (ör. CMake hazırlanırken) en çok kaç saniye beklensin. Bekleyen işler 'Kamil: VS işleri' ile iptal edilebilir"});
        s.add(std::move(d));
    }
    return s;
}

}  // namespace

const Schema& builtin_schema() {
    static const Schema schema = build();
    return schema;
}

}  // namespace kamil
