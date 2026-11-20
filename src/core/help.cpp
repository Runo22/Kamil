#include "core/help.h"

#include <vector>

namespace kamil {

namespace {

const HelpTopic kTopics[] = {
    {"start", "Getting started", "Başlarken",
     R"(Press `Alt+Space` (`general.hotkey`) and type: apps, projects, files, folders and commands appear as you type.
- `Enter` opens, `Shift+Enter` runs as administrator, `Ctrl+Enter` shows the file in Explorer.
- `Ctrl+K` (or `→` at the end of the text): everything you can do with the selected result.
- `Tab` completes the name, `Alt+1` … `Alt+9` open a visible result, `Esc` closes.
- Kamil learns: what you pick for a text comes first next time.)",
     R"(`Alt+Space` (`general.hotkey`) ile aç ve yaz: uygulamalar, projeler, dosyalar, klasörler ve komutlar yazdıkça gelir.
- `Enter` açar, `Shift+Enter` yönetici olarak çalıştırır, `Ctrl+Enter` dosyayı Gezgin'de gösterir.
- `Ctrl+K` (ya da metnin sonunda `→`): seçili sonuçla yapılabilecek her şey.
- `Tab` adı tamamlar, `Alt+1` … `Alt+9` görünen sonucu açar, `Esc` kapatır.
- Kamil öğrenir: bir yazışta seçtiğin sonuç sonraki sefer en üste gelir.)",
     R"(general:
  hotkey: Alt+Space        # Win+Space, Ctrl+Space, F12 ...
  language: auto           # auto | en | tr
appearance:
  theme: auto              # auto | dark | light
  max_rows: 8
)"},
    {"settings", "Settings", "Ayarlar",
     R"(Tray icon → **Edit settings** opens `settings.yaml`; it applies the moment you save.
- A wrong value is reported in a balloon with its line number; the rest keeps working.
- Write Windows paths in **single quotes**: `'D:\src'`.
- **Kamil: Settings examples** opens `settings.example.yaml`: every setting with its explanation, always up to date (your own `settings.yaml` is never rewritten, so new settings are not added to it).
- Copy only what you change into `settings.yaml`; everything else keeps its default.)",
     R"(Tray simgesi → **Ayarları düzenle** `settings.yaml`'ı açar; kaydettiğin anda uygulanır.
- Hatalı değer, satır numarasıyla balonda bildirilir; gerisi çalışmaya devam eder.
- Windows yollarını **tek tırnakla** yaz: `'D:\src'`.
- **Kamil: Ayar örnekleri** `settings.example.yaml`'ı açar: her ayar açıklamasıyla, her zaman güncel (kendi `settings.yaml`'ın yeniden yazılmaz, yeni ayarlar ona eklenmez).
- `settings.yaml`'a yalnızca değiştirdiğini kopyala; gerisi varsayılanında kalır.)",
     nullptr},
    {"files", "Files, folders and bookmarks", "Dosyalar, klasörler ve yer imleri",
     R"(Files under `search.folders` are found as you type; scripts run with `Enter` (`Alt+E` edits).
- **Bookmarks** give any folder or file a name, hidden folders too (`~\.conan`); the `keyword` puts it first.
- **Aliases**: typing the alias exactly puts that app first.)",
     R"(`search.folders` altındaki dosyalar yazdıkça bulunur; script'ler `Enter` ile çalışır (`Alt+E` düzenler).
- **Yer imleri** herhangi bir klasöre ya da dosyaya ad verir, gizli klasörlere de (`~\.conan`); `keyword` onu en üste getirir.
- **Takma adlar**: takma ad tam yazılınca o uygulama en üste gelir.)",
     R"(search:
  folders:
    - path: 'D:\tools\scripts'
      priority: 60
    - path: 'D:\src'
      depth: 4
      include: ['*.py', '*.bat', '*.ps1']
  bookmarks:
    - name: Conan cache
      path: '~\.conan'
      keyword: conan
  aliases:
    - alias: code
      target: Visual Studio Code
)"},
    {"browse", "Browse into a folder", "Klasörde gezinme",
     R"(With a folder selected, press `\`: its contents are listed (hidden ones too). Typing filters, `\` on a sub folder goes deeper, `Backspace` goes back up.
- You can also type or paste a path: `C:\`, `~\.conan\`, `%APPDATA%\`.)",
     R"(Bir klasör seçiliyken `\`'a bas: içindekiler listelenir (gizliler de). Yazdıkça süzülür, alt klasörde `\` bir alta iner, `Backspace` bir üste çıkar.
- Yolu doğrudan yazabilir ya da yapıştırabilirsin: `C:\`, `~\.conan\`, `%APPDATA%\`.)",
     nullptr},
    {"lines", "Open a file at a line", "Dosyayı satırında aç",
     R"(Type or paste `main.cpp:120`, `a.cpp(12,5)` or a whole compiler error / Python traceback line: `Enter` opens the file at that line (in the Visual Studio that has the project open, otherwise VS Code).
- A path or location you **copied** in the last 2 minutes is the first result when Kamil opens.
- In **Kamil Console**, double-click an error line (or `Enter`, `F8`) to jump to it.)",
     R"(`main.cpp:120`, `a.cpp(12,5)` ya da bir derleyici hatası / Python traceback satırını yaz veya yapıştır: `Enter` dosyayı o satırda açar (proje VS'te açıksa VS'te, değilse VS Code'da).
- Son 2 dakikada **kopyaladığın** bir yol ya da konum, Kamil açılınca ilk sonuçtur.
- **Kamil Console**'da hata satırına çift tıkla (ya da `Enter`, `F8`): o satıra gider.)",
     R"(search:
  clipboard_suggestion: true
)"},
    {"projects", "CMake projects and Visual Studio", "CMake projeleri ve Visual Studio",
     R"(Git repositories and CMake projects under `dev.project_roots` are results; `Ctrl+K` on one: Configure, Build, Debug, Run, presets, targets, COM port.
- Quick keys on a project: `Alt+B` build, `Alt+D` debug, `Alt+R` run, `Alt+C` configure.
- **Kamil: VS jobs** lists and cancels running Visual Studio jobs.)",
     R"(`dev.project_roots` altındaki git depoları ve CMake projeleri sonuç olarak çıkar; birinde `Ctrl+K`: Configure, Build, Debug, Run, preset'ler, hedefler, COM port.
- Projede hızlı tuşlar: `Alt+B` build, `Alt+D` debug, `Alt+R` çalıştır, `Alt+C` configure.
- **Kamil: VS işleri** çalışan Visual Studio işlerini listeler ve iptal eder.)",
     R"(dev:
  project_roots: ['D:\src', 'D:\work']
  default_vs: vs2026         # vs2026 | vs2022
  repo_action: vs            # Enter on a project: vs | code | explorer | terminal
  default_args: '--port {com} --baud 115200'
)"},
    {"git", "Git", "Git",
     R"(`Ctrl+K` on a repository, then type `git`: status, switch branch, fetch, pull (`--ff-only`), push, log.
- Status and log open in Kamil Console; the others report with a notification.
- The branch and changes of the selected project are shown in the footer.)",
     R"(Bir depoda `Ctrl+K`, sonra `git` yaz: durum, dal değiştir, fetch, pull (`--ff-only`), push, geçmiş.
- Durum ve geçmiş Kamil Console'da açılır; diğerleri sonucu bildirimle söyler.
- Seçili projenin dalı ve değişiklikleri alt çubukta görünür.)",
     nullptr},
    {"workspaces", "Workspaces", "Çalışma alanları",
     R"(One name opens several projects, folders, files and apps. An entry with a `branch` is checked first: if the repository is on another branch, Kamil offers to switch it.
- `Ctrl+K` on a workspace lists its entries with their current branches.)",
     R"(Tek bir ad birden çok proje, klasör, dosya ve uygulamayı açar. `branch` verilen girdi önce denetlenir: depo başka daldaysa Kamil dalı değiştirmeyi önerir.
- Çalışma alanında `Ctrl+K` girdileri şu anki dallarıyla listeler.)",
     R"(workspaces:
  - name: Sensor work
    keyword: sen
    open:
      - { path: 'D:\src\sensor', with: vs, branch: develop }
      - { path: 'D:\src\sensor-lib', with: code }
      - { item: Tera Term }
)"},
    {"conan", "Conan packages", "Conan paketleri",
     R"(Packages in the Conan 1 cache are results: type a package name, `conan` (all packages first) or `conan fmt` (only packages).
- `Enter` opens the package folder when it has several versions, otherwise the folder with export, package, source.)",
     R"(Conan 1 önbelleğindeki paketler sonuç olarak çıkar: paket adını, `conan` (önce tüm paketler) ya da `conan fmt` (yalnızca paketler) yaz.
- `Enter`, paketin birden çok sürümü varsa paket klasörünü, yoksa export, package, source klasörünü açar.)",
     R"(dev:
  conan: true
  conan_home: ''             # empty: %CONAN_USER_HOME%\.conan, else %USERPROFILE%\.conan
)"},
    {"commands", "Your own commands", "Kendi komutların",
     R"(Commands you define are results like any other; `keyword` puts one first, `hotkey` runs it while Kamil is open.
- Placeholders: `{input}` (asked), `{clip}`, `{project}`, `{preset}`, `{exe}`, `{com}` …; `console: hidden` shows the result as a notification.)",
     R"(Tanımladığın komutlar diğer sonuçlar gibi çıkar; `keyword` en üste getirir, `hotkey` Kamil açıkken çalıştırır.
- Yer tutucular: `{input}` (sorulur), `{clip}`, `{project}`, `{preset}`, `{exe}`, `{com}` …; `console: hidden` sonucu bildirimle gösterir.)",
     R"(commands:
  - name: Flash device
    run: 'py D:\tools\flash.py --port {com} --file "{exe}"'
    keyword: fl
    hotkey: Ctrl+Shift+F
    console: hidden
)"},
    {"spell", "Spell check", "Yazım denetimi",
     R"(Copy a text, open Kamil, type `spell` and `Enter`: mistakes are underlined in English and Turkish at once (Windows' own dictionaries).
- Right click or `Ctrl+.`: suggestions. `Tab` applies the first, `F8` next mistake, `Ctrl+Shift+F` fixes all.
- `Ctrl+Enter` copies the text and pastes it back into the program you came from.
- `spell recieved` in the search lists suggestions right away.)",
     R"(Metni kopyala, Kamil'i aç, `spell` yaz ve `Enter`: hatalar İngilizce ve Türkçe birlikte işaretlenir (Windows'un kendi sözlükleri).
- Sağ tık ya da `Ctrl+.`: öneriler. `Tab` ilkini uygular, `F8` sonraki hata, `Ctrl+Shift+F` hepsini düzeltir.
- `Ctrl+Enter` metni kopyalar ve geldiğin programa geri yapıştırır.
- Aramada `spell recieved` önerileri doğrudan listeler.)",
     R"(spell:
  keywords: ['spell', 'yaz']
  languages: ['en-US', 'tr-TR']
  paste_back: true
)"},
    {"apps", "Apps", "Uygulamalar",
     R"(Programs in `apps.bring_running` are not started twice: `Enter` brings the running window to the current desktop.
- `search.exclude_apps` hides apps by name.)",
     R"(`apps.bring_running` listesindeki programlar ikinci kez açılmaz: `Enter` açık pencereyi bulunduğun masaüstüne getirir.
- `search.exclude_apps` uygulamaları adıyla gizler.)",
     R"(apps:
  bring_running: ['Code.exe', 'TeraTerm*', 'Outlook']
search:
  exclude_apps: ['*uninstall*']
)"},
    {"trouble", "When something does not work", "Bir şey çalışmazsa",
     R"(**Kamil: Diagnostics** shows what Kamil found: folders, index, Visual Studio, Conan cache, spell check languages.
- **Kamil: Open log** opens `kamil.log`; send it together with a screenshot.
- **Kamil: Rescan** reads apps, projects and files again.)",
     R"(**Kamil: Tanılama** Kamil'in bulduklarını gösterir: klasörler, indeks, Visual Studio, Conan önbelleği, yazım denetimi dilleri.
- **Kamil: Günlüğü aç** `kamil.log`'u açar; ekran görüntüsüyle birlikte gönder.
- **Kamil: Yeniden tara** uygulamaları, projeleri ve dosyaları yeniden okur.)",
     nullptr},
};

void escape_into(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
}

// `code` and **bold** inside an already escaped line.
std::string inline_markup(std::string_view line) {
    std::string escaped;
    escape_into(escaped, line);
    std::string out;
    bool code = false, bold = false;
    for (size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] == '`') {
            out += code ? "</code>" : "<code>";
            code = !code;
        } else if (!code && escaped.compare(i, 2, "**") == 0) {
            out += bold ? "</b>" : "<b>";
            bold = !bold;
            ++i;
        } else {
            out += escaped[i];
        }
    }
    if (code) out += "</code>";
    if (bold) out += "</b>";
    return out;
}

std::string body_html(std::string_view body) {
    std::string out;
    bool list = false;
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string_view::npos) end = body.size();
        const std::string_view line = body.substr(start, end - start);
        start = end + 1;
        if (line.empty()) {
            if (end == body.size()) break;
            continue;
        }
        if (line.substr(0, 2) == "- ") {
            if (!list) out += "<ul>";
            list = true;
            out += "<li>" + inline_markup(line.substr(2)) + "</li>";
        } else {
            if (list) out += "</ul>";
            list = false;
            out += "<p>" + inline_markup(line) + "</p>";
        }
        if (end == body.size()) break;
    }
    if (list) out += "</ul>";
    return out;
}

}  // namespace

std::span<const HelpTopic> help_topics() { return kTopics; }

std::string help_html(bool tr, std::string_view version) {
    std::string toc, sections;
    for (const auto& t : kTopics) {
        const char* title = tr ? t.title_tr : t.title_en;
        toc += std::string("<a href=\"#") + t.id + "\">";
        escape_into(toc, title);
        toc += "</a>";
        sections += std::string("<section id=\"") + t.id + "\"><h2>";
        escape_into(sections, title);
        sections += "</h2>" + body_html(tr ? t.body_tr : t.body_en);
        if (t.yaml) {
            sections += "<div class=\"ex\"><div class=\"exbar\"><span>settings.yaml</span><button onclick=\"cp(this)\">";
            sections += tr ? "Kopyala" : "Copy";
            sections += "</button></div><pre>";
            escape_into(sections, t.yaml);
            sections += "</pre></div>";
        }
        sections += "</section>";
    }
    std::string page = R"(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kamil — )";
    page += tr ? "Yardım" : "Help";
    page += R"(</title><style>
:root{--bg:#f6f5f3;--card:#fff;--text:#1d1f22;--muted:#6b7078;--line:#e4e1dc;--code:#f0eeea;--accent:#2f6fdb}
@media (prefers-color-scheme:dark){:root{--bg:#1b1d21;--card:#24272c;--text:#e6e8eb;--muted:#9aa1aa;--line:#33373d;--code:#2c3036;--accent:#6ea0ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.55 "Segoe UI",system-ui,sans-serif}
header{position:sticky;top:0;background:var(--bg);border-bottom:1px solid var(--line);padding:14px 24px;display:flex;gap:16px;align-items:center;z-index:1}
header h1{font-size:19px;margin:0;font-weight:600}header .v{color:var(--muted);font-size:13px}
header input{margin-left:auto;width:280px;max-width:45vw;padding:7px 11px;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--text);font:inherit}
.wrap{display:flex;max-width:1100px;margin:0 auto;gap:28px;padding:20px 24px 60px}
nav{flex:none;width:210px;position:sticky;top:76px;align-self:flex-start;display:flex;flex-direction:column;gap:2px}
nav a{color:var(--muted);text-decoration:none;padding:5px 10px;border-radius:7px;font-size:14px}nav a:hover{background:var(--card);color:var(--text)}
main{flex:1;min-width:0;display:flex;flex-direction:column;gap:16px}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px 20px;scroll-margin-top:80px}
h2{font-size:17px;margin:0 0 6px}p{margin:6px 0}ul{margin:6px 0;padding-left:20px}li{margin:3px 0}
code{background:var(--code);padding:1px 5px;border-radius:5px;font:13px Consolas,"Cascadia Mono",monospace}
.ex{margin-top:10px;border:1px solid var(--line);border-radius:9px;overflow:hidden}
.exbar{display:flex;justify-content:space-between;align-items:center;padding:5px 10px;background:var(--code);color:var(--muted);font-size:12.5px}
.exbar button{border:1px solid var(--line);background:var(--card);color:var(--text);border-radius:6px;padding:2px 10px;font:inherit;cursor:pointer}
pre{margin:0;padding:10px 14px;overflow:auto;font:13px/1.5 Consolas,"Cascadia Mono",monospace}
.hide{display:none}@media (max-width:760px){nav{display:none}}
</style></head><body><header><h1>Kamil — )";
    page += tr ? "Yardım" : "Help";
    page += "</h1><span class=\"v\">";
    escape_into(page, version);
    page += "</span><input id=\"q\" placeholder=\"";
    page += tr ? "Ara…" : "Search…";
    page += "\" autofocus></header><div class=\"wrap\"><nav>" + toc + "</nav><main>" + sections + "</main></div>";
    page += R"(<script>
function cp(b){const t=b.closest('.ex').querySelector('pre').innerText;navigator.clipboard.writeText(t).then(()=>{const o=b.textContent;b.textContent='✓';setTimeout(()=>b.textContent=o,1200)})}
document.getElementById('q').addEventListener('input',e=>{const q=e.target.value.toLowerCase();document.querySelectorAll('section').forEach(s=>{const hit=!q||s.innerText.toLowerCase().includes(q);s.classList.toggle('hide',!hit);const a=document.querySelector('nav a[href="#'+s.id+'"]');if(a)a.classList.toggle('hide',!hit)})})
</script></body></html>
)";
    return page;
}

}  // namespace kamil
