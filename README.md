# Kamil

Windows için offline, geliştirici odaklı başlatıcı. Bir kısayolla açılır; uygulamaları, projeleri, dosyaları ve
script'leri yazdıkça bulur, seçimlerinden öğrenir. CMake projelerini Visual Studio'ya **configure / build / debug**
ettirir, çıktıları kendi konsolunda gösterir. İnternet gerektirmez, tek bir `Kamil.exe`'dir.

Arayüz İngilizce ve Türkçe; varsayılan olarak Windows'un görüntü dilini izler (`general.language: auto | en | tr`).
Tasarım ve yol haritası: [`docs/TASARIM.md`](docs/TASARIM.md)

**İçindekiler:**
[Başlarken](#başlarken) ·
[Açma kısayolu](#açma-kısayolu-altspace--winspace) ·
[Arama penceresi](#arama-penceresi) ·
[Projeler](#projeler-cmake--visual-studio) ·
[Her yerden çalışan kısayollar](#her-yerden-çalışan-kısayollar-ve-özel-komutlar) ·
[Kamil Console](#kamil-console) ·
[Dosyalar ve script'ler](#dosyalar-ve-scriptler) ·
[Dahili komutlar](#dahili-komutlar) ·
[Ayarlar](#ayarlar) ·
[Sorun giderme](#sorun-giderme) ·
[Geliştirme](#geliştirme)

---

## Başlarken

1. `Kamil.exe`'yi istediğin bir klasöre koy ve çalıştır (kurulum yok, ek DLL gerekmez). Derlenmiş son sürüm GitHub
   Actions'taki her başarılı çalıştırmada *Kamil-x64-release* olarak indirilebilir.
2. Saatin yanında Kamil simgesi belirir ve bir balon açma kısayolunu söyler (varsayılan **`Alt+Space`**).
3. İlk çalıştırmada açıklamalı bir `settings.yaml` oluşturulur: tray menüsü → **Edit settings**. Dosya kaydedildiği
   anda uygulanır.
4. Projelerinin bulunması için `dev.project_roots`, dosya/script araması için `search.folders` ayarlarını kendi
   klasörlerine göre düzenle (bkz. [Ayarlar](#ayarlar)).
5. Oturum açılınca kendiliğinden başlasın istersen: `general.start_with_windows: true`.

Kamil zaten çalışırken `Kamil.exe`'yi tekrar açmak yeni bir kopya başlatmaz, açık olanın penceresini gösterir.

## Açma kısayolu (`Alt+Space` / `Win+Space`)

Kısayol `settings.yaml` içindeki `general.hotkey` ayarıdır; kaydedince hemen değişir:

```yaml
general:
  hotkey: Win+Space     # veya Alt+Space (varsayılan), Ctrl+Space, Ctrl+Alt+K, F12 ...
```

- **`Win+Space`**: Windows bu kısayolu normalde *klavye düzenini değiştirmek* için kullanır. Kamil onu devralır
  (düzen değişmez, Başlat menüsü de açılmaz); düzeni değiştirmek için **`Alt+Shift`** yine çalışır.
  Windows tuşlu kısayollar klavye kancasıyla yakalanır. Bu yüzden **yönetici olarak çalışan bir pencere** (ör. "Run as
  administrator" ile açılmış bir program) öndeyken çalışmazlar; o durumda Kamil'i de yönetici olarak çalıştır ya da
  `Alt+Space` kullan.
- Kısayol başka bir program tarafından alınmışsa (ör. PowerToys Run da `Alt+Space` kullanır) tray balonu uyarır;
  başka bir kısayol seç.
- Tray simgesine tıklamak da pencereyi açıp kapatır.

## Arama penceresi

Yazmaya başla: sonuçlar her tuşta güncellenir. Boş pencerede en sık / son kullandıkların listelenir.

### Tuşlar

| Tuş | İşlev |
|---|---|
| `Enter` | Seçileni aç / çalıştır |
| `Shift+Enter` | Yönetici olarak çalıştır |
| `Ctrl+Enter` | Dosya konumunu Gezgin'de göster (projelerde: klasörü Gezgin'de aç) |
| `Alt+1` … `Alt+9` | Görünen n. sonucu doğrudan aç |
| `↑` `↓`, `Ctrl+J`, fare tekerleği | Sonuçlarda gezin |
| `PgUp` `PgDn`, `Ctrl+Home` `Ctrl+End` | Sayfa sayfa / en başa / en sona |
| `Tab` | Seçili öğenin adını arama kutusuna yaz (tamamla) |
| `Ctrl+K` veya metnin sonunda `→` | **Eylem paneli**: seçili öğe için yapılabilecek her şey |
| `Esc` veya boş filtrede `←` | Eylem panelinden geri dön; ana listede pencereyi kapat |
| `Ctrl+C` | Metin seçili değilse seçili öğenin yolunu kopyala |
| `Ctrl+A` `Ctrl+V` `Ctrl+X`, `Ctrl+Backspace` | Arama kutusunda düzenleme (`Ctrl+←/→` kelime kelime) |
| `Alt+harf` | Seçili öğeye özel hızlı eylem (aşağıdaki tablolar) |
| `Alt+F4` | Pencereyi kapat |

Fareyle de seçilebilir; başka bir yere tıklamak pencereyi kapatır (`general.hide_on_focus_loss`).

### Ne aranır, nasıl eşleşir

- **Uygulamalar:** Başlat Menüsü ve Store uygulamaları, kendi ikonlarıyla.
- **Projeler:** `dev.project_roots` altındaki git depoları ve CMake projeleri (dal adıyla).
- **Dosyalar ve klasörler:** `search.folders` altındaki her şey (Everything gerekmez); yeni dosyalar anında görünür.
- **Dahili komutlar** (`Kamil: …`) ve kendi **özel komutların**.

Eşleştirme bulanıktır: harflerin sırayla geçmesi yeter, kelime başları ve baş harfler öne çıkar (`ws` → *Windows
Security*, `vsc` → *Visual Studio Code*). Türkçe karakterler katlanır: `calisma` → *Çalışma Planı*. Dahili komutlar iki
dilde de bulunur (`settings` = `ayarlar`, `jobs` = `iptal`).

Kamil seçimlerinden öğrenir: bir yazışta sık seçtiğin sonuç o yazışta en üste çıkar (`search.learning`; veriler sadece
bu bilgisayarda, `usage.tsv`). Sıfırlamak için *Kamil: Forget what was learned*. Takma ad tanımlayabilirsin:
`search.aliases` (ör. `code` → *Visual Studio Code*).

### Alt bilgi çubuğu

Pencerenin altındaki satır solda seçili öğenin bağlamını (git dalı, değişiklik sayısı, preset, hedef, COM port, çalışan
VS işi), sağda o anda geçerli tuşları gösterir. Sağdaki **tuş çiplerine tıklamak** tuşa basmakla aynıdır.

## Projeler (CMake + Visual Studio)

**Proje sayılan klasörler:** `dev.project_roots` altındaki git depoları ve CMake projeleri, ayrıca aramada nereden
bulunursa bulunsun (ör. `search.folders` içinden) bir **CMake proje kökü**: `project(...)` çağıran bir
`CMakeLists.txt` ya da `CMakePresets.json` içeren klasör. Büyük bir projenin `add_subdirectory` parçası olan alt
klasörler proje sayılmaz. Aramada **CMake** yazısıyla görünür; bir klasörün altındaki tüm projelerin her zaman
listelenmesini istersen o klasörü `dev.project_roots`'a ekle.

Kamil derleme yapmaz: komutları **Visual Studio'ya** (2026 veya 2022, `dev.default_vs`) COM üzerinden gönderir, VS
derler, Kamil bitişi izler ve sonucu gösterir. Projeler VS'te **Open Folder** ile açılır.

### Bir CMake projesi seçiliyken

| Tuş | İşlev |
|---|---|
| `Alt+C` | **Configure**: VS'te CMake configure (Generate Cache) |
| `Alt+B` | **Build**: VS'te Build All; çıktı canlı olarak [Kamil Console](#kamil-console)'a akar |
| `Alt+D` | **Debug**: VS'te derle → programı duraklatılmış başlat → VS debugger'ını bağla → devam (`main`'deki breakpoint'ler de tutar) |
| `Alt+R` | **Çalıştır**: son derlenen exe'yi argümanlarla Kamil Console'da başlat |
| `Enter` | Klasörü `dev.repo_action` ile aç (varsayılan: Visual Studio) |
| `Ctrl+K` | Eylem paneli |

VS bu klasörü açmamışsa Kamil `devenv "<klasör>"` ile açar ve hazır olmasını bekler (en çok `dev.vs_wait_seconds`,
varsayılan 90 sn). Neyi beklediği alt bilgide yazar ("VS açılıyor", "CMake hazırlanıyor", "VS başka build yapıyor").

### Eylem paneli (`Ctrl+K`)

Sırasıyla: çalışan işi **iptal et** (varsa) · **Configure** · **Build** · **Debug** · **Run** · *Open in Visual Studio*
· *Open in VS Code* · **Preset** seç · **Hedef** seç (birden çok exe varsa) · **COM port** seç · **Argümanlar** ·
diğer preset'lerin build'lerini ayrı ayrı çalıştır · Rebuild · Önbelleği silip yeniden yapılandır · Gezgin, terminal,
Git Bash, Git GUI, yolu / dal adını kopyala. Listede yazarak süzebilirsin; `Tab` tamamlar.

- **Preset:** `CMakePresets.json` okunur. Seçmezsen VS'te en son configure edilen preset kullanılır. Open Folder'da VS
  **kendi seçili preset'ini** derler; Kamil'deki seçim farklıysa uyarır.
- **COM port:** bağlı seri portlar dostu adlarıyla listelenir; port numarası değişse de cihaz VID/PID ile hatırlanır.
- **Argümanlar:** proje başına şablon, ör. `--port {com} --baud 115200`. Yer tutucular: `{com}` `{preset}`
  `{target}` `{config}` `{project}`. Boşsa `dev.default_args` kullanılır.
- Seçimler proje başına hatırlanır (`projects.tsv`).

### VS iş kuyruğu

VS işleri sırayla çalışır; aynı iş ikinci kez sıraya alınmaz. Alt bilgi çalışan işi ve `+N sırada` sayısını gösterir.

- **Kamil: VS jobs** (veya tray menüsü → *VS jobs*): çalışan ve sıradaki işler; `Enter` seçileni iptal eder, çalışan
  build VS'te de durdurulur.
- Tray menüsü → *Cancel all VS jobs*.

## Her yerden çalışan kısayollar ve özel komutlar

Bu kısayollar Kamil'i açmadan, **her uygulamanın içinden** (VS'in içinden de) çalışır:

| Kısayol | Varsayılan komut |
|---|---|
| `Ctrl+Alt+B` | **Build last project**: son kullanılan CMake projesini derler |
| `Ctrl+Alt+D` | **Debug last project**: derler ve debug eder |
| — | **Configure last project** (aramada adıyla bulunur; istersen kısayol ver) |

*Son kullanılan proje*, Kamil'den en son bir eylem yaptığın (ya da `Enter` ile açtığın) CMake projesidir.

Kısayollar ve komutlar `settings.yaml` içindeki `commands:` listesindedir. Her komut aramada adıyla da çıkar:

```yaml
commands:
  - name: Build last project
    action: build             # configure | build | rebuild | reconfigure | debug | run | jobs | cancel-jobs
    hotkey: Ctrl+Alt+B
  - name: Flash device
    run: 'py D:\tools\flash.py --port {com} --file "{exe}"'   # cmd ile çalışır: | ve && kullanılabilir
    hotkey: Win+F             # Win'li kısayollar da olur
    console: hidden           # keep (pencere açık kalır) | close | hidden (Kamil Console'a yazar, sonucu bildirir)
  - name: Log ara
    run: 'findstr /i "{input}" D:\logs\*.log'   # {input} çalıştırılırken sorulur
    dir: 'D:\logs'
    confirm: false            # true: çalıştırmadan önce sor
    admin: false              # true: yönetici olarak
```

Yer tutucular (`run` ve `dir` içinde): `{input}` (sorulur), `{clip}` (pano), `{date}`, `{time}` ve son projeden
`{project}` `{project_dir}` `{preset}` `{target}` `{config}` `{exe}` `{exe_dir}` `{com}` `{args}`.

Bir kısayol başka bir program tarafından kullanılıyorsa tray balonu bir kez uyarır. Türkçe klavyede `AltGr`, `Ctrl+Alt`
olarak gelir: `Ctrl+Alt+harf` kısayolları o harfin AltGr karakterini (ör. `AltGr+Q` → `@`) yazdırmaz hale getirebilir;
sorun olursa kısayolu değiştir.

## Kamil Console

`Alt+R` ile çalıştırılan programların çıktısı, Visual Studio build'lerinin **Build** bölmesi ve `console: hidden`
komutlar **Kamil Console** penceresinde sekme sekme toplanır. Açmak için: *Kamil: Console* veya tray menüsü → *Console*.

- Her preset/hedef kendi sekmesinde (`SensorUI [x64-debug]` ve `SensorUI [x64-release]` yan yana). Aynı iş tekrar
  çalışınca biten sekmesi yeniden kullanılır.
- MSVC, GCC/Clang hataları ve Python traceback satırları renklidir; dosya satırına gidilebilir: proje VS'te açıksa
  **VS'te**, değilse **VS Code**'da açılır.
- Build başarısız olursa pencere kendiliğinden açılır ve ilk hata seçili gelir (`dev.console_on_build: on_error |
  always | never`).
- Klavyeden girdi okuyan programlar için `dev.run_in: console` eski davranışa (her çalıştırma kendi konsol
  penceresinde) döner.

| Tuş / fare | İşlev |
|---|---|
| `F8` / `Shift+F8` | Sonraki / önceki hataya git ve dosyayı o satırda aç |
| Çift tıklama, `Enter` | Seçili satırdaki dosyayı aç |
| `F5`, *Run again* | Aynı çalıştırmayı / build'i tekrarla |
| *Stop* | Kamil'in başlattığı programı durdur (build sekmesinde VS işini iptal et) |
| `Ctrl+C` | Seçili satırları kopyala (seçim yoksa tümünü) |
| `Ctrl+A` | Tümünü seç |
| Tıklama, `Shift+tıklama`, `↑` `↓` (`Shift` ile) | Satır seç / seçimi genişlet |
| `PgUp` `PgDn` `Home` `End`, fare tekerleği | Kaydır (`End` en alta iner ve yeni çıktıyı takip eder) |
| `←` `→`, `Shift+tekerlek` | Uzun satırlarda yatay kaydır |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Sonraki / önceki sekme |
| `Ctrl+W`, orta tıklama, sekmedeki `✕` | Sekmeyi kapat (çalışan program varsa önce sorar) |
| `Esc` | Pencereyi gizle (sekmeler kaybolmaz) |

## Dosyalar ve script'ler

`search.folders` altındaki dosya ve klasörler aramada çıkar, `priority` kadar öne alınır. Klasörler izlenir: yeni, silinen
ve yeniden adlandırılan dosyalar anında güncellenir. İndeks diske kaydedilir; Kamil açılır açılmaz arama yapılabilir.

```yaml
search:
  folders:
    - path: 'D:\tools\scripts'
      priority: 60
    - path: 'D:\src'
      priority: 20
      depth: 4
      include: ['*.py', '*.pyw', '*.bat', '*.ps1', '*.sh']   # boş: tüm dosyalar
```

| Dosya | `Enter` | `Alt+E` | `Alt+R` | `Ctrl+K` |
|---|---|---|---|---|
| `.bat` `.cmd` | `cmd` ile çalıştırır | düzenle | çalıştır | yönetici olarak, konum, bu klasörde terminal, yolu kopyala |
| `.ps1` | PowerShell (`-ExecutionPolicy Bypass`) ile çalıştırır | düzenle | çalıştır | 〃 |
| `.py` / `.pyw` | `py` / `pyw` (veya `scripts.python`) ile çalıştırır | düzenle | çalıştır | 〃 |
| `.sh` | Git for Windows `bash` ile çalıştırır | düzenle | çalıştır | 〃 |
| diğer dosyalar | varsayılan uygulamayla açar | düzenle | — | birlikte aç…, konum, yolu kopyala |
| klasörler | Gezgin'de açar | — | — | VS Code, Visual Studio (Open Folder), terminal |

- `scripts.default_action: edit` → `Enter` düzenler, `Alt+R` çalıştırır.
- `scripts.editor`: `auto` (VS Code varsa o), `code`, `notepad`, `default`.
- `scripts.keep_console_open: false` → script bitince konsol kapanır.

## Dahili komutlar

Aramada yazarak bulunur (İngilizce veya Türkçe adıyla):

| Komut | İşlev |
|---|---|
| Kamil: Edit settings / *Ayarları düzenle* | `settings.yaml`'ı düzenleyicide açar |
| Kamil: Open settings folder / *Ayar klasörünü aç* | Ayar, öğrenme ve önbellek dosyalarının klasörü |
| Kamil: Reload settings / *Ayarları yeniden yükle* | `settings.yaml`'ı yeniden okur (normalde kaydedince otomatik) |
| Kamil: Rescan / *Yeniden tara* | Uygulamaları, projeleri ve dosyaları yeniden tarar |
| Kamil: VS jobs / *VS işleri* | Çalışan ve sıradaki VS işleri; iptal |
| Kamil: Console / *Konsol* | Kamil Console'u açar |
| Kamil: Forget what was learned / *Öğrenilenleri sıfırla* | Sık kullanılanları ve arama tercihlerini siler |
| Kamil: Test VS connection / *VS bağlantısını test et* | VS kurulumları, açık örnekler, yönetici durumu, CMake komut adları raporu |
| Kamil: Diagnostics / *Tanılama* | Sürüm, klasörler, indeks, arama hızı, VS, çökme dökümleri raporu |
| Kamil: Open log / *Günlüğü aç* | `kamil.log` |
| Kamil: Quit / *Çıkış* | Kamil'i kapatır |

**Tray menüsü** (simgeye sağ tık): Kamil'i aç · VS işleri ve tümünü iptal (iş varsa) · Edit settings · Open settings
folder · Reload settings · ayar sorunları (varsa) · Rescan · Console · Open log · Quit.

## Ayarlar

| Mod | Konum |
|---|---|
| Normal | `%APPDATA%\Kamil\settings.yaml` (önbellek ve günlük: `%LOCALAPPDATA%\Kamil\cache`) |
| Taşınabilir | `Kamil.exe` yanında `data\` klasörü veya `portable.txt` dosyası varsa her şey `data\` altında |

Dosya kaydedildiği anda uygulanır. Hatalı değer varsayılanına döner ve tray balonunda satır numarasıyla bildirilir;
YAML sözdizimi bozuksa önceki ayarlar korunur. Her ayarın açıklaması dosyanın içinde yazar. VS Code + YAML eklentisiyle
açarsan `schema\settings.json` sayesinde otomatik tamamlama ve hata gösterimi çalışır. Windows yollarını **tek
tırnakla** yaz: `'D:\src'`.

En çok kullanılanlar:

```yaml
general:
  language: auto            # auto | en | tr
  hotkey: Alt+Space         # Win+Space de olur
  start_with_windows: false
appearance:
  theme: auto               # auto | dark | light
  max_rows: 8
search:
  folders: [...]            # yukarıda
  exclude_apps: ['*uninstall*', '*kaldır*']
  aliases:
    - alias: code
      target: Visual Studio Code
dev:
  default_vs: vs2026        # vs2026 | vs2022
  devenv_path: ''           # boş: otomatik (vswhere / standart klasörler)
  project_roots: ['D:\src', 'D:\work']
  repo_action: vs           # projede Enter: vs | code | explorer | terminal
  terminal: wt              # wt | cmd | powershell | git-bash
  default_args: '--port {com} --baud 115200'
  build_before_debug: true
  run_in: kamil             # kamil (Console sekmesi) | console (ayrı pencere)
  console_on_build: on_error
  vs_wait_seconds: 90
commands: [...]             # yukarıda
```

## Sorun giderme

| Sorun | Ne yapmalı |
|---|---|
| Kısayol çalışmıyor | Tray balonu "kullanılıyor" dediyse başka kısayol seç. `Win+…` kısayolları yönetici olarak çalışan bir pencere öndeyken çalışmaz. *Kamil: Diagnostics* kısayolun kayıtlı olup olmadığını gösterir. |
| VS'e gönderilen komut olmuyor | *Kamil: VS jobs* işin neyi beklediğini gösterir; takılan işi oradan iptal et. *Kamil: Test VS connection* raporu ve `kamil.log` ayrıntıyı verir. |
| "Visual Studio … yönetici olarak çalışıyor" | Normal çalışan Kamil, yönetici olarak açılmış VS'i yönetemez (Windows kısıtı). VS'i normal aç ya da Kamil'i de yönetici olarak çalıştır. |
| "Visual Studio bulunamadı" / *Open in Visual Studio* uyarı veriyor | `dev.devenv_path` ile `devenv.exe` yolunu ver. |
| Dosya / script aramada çıkmıyor | Klasörü `search.folders`'a ekle; `exclude_dirs` ve `depth` ayarlarına bak; indeks `max_files`'ta kesildiyse *Diagnostics* söyler. |
| Program çıktısı geç geliyor / Türkçe karakterler bozuk | Python'a `PYTHONUNBUFFERED` verilir; diğer programlar çıktıyı kendileri tamponluyor olabilir. Kodlama UTF-8 veya OEM olarak otomatik seçilir. |
| Kamil kapandı | Bir sonraki açılışta balon çıkar; döküm (`crash-*.dmp`) önbellek klasöründedir. *Diagnostics* raporuyla birlikte gönder. |

Kamil'in dosyaları:

| Dosya | Yer | İçerik |
|---|---|---|
| `settings.yaml`, `schema\settings.json` | ayar klasörü | Ayarlar ve editör şeması |
| `usage.tsv`, `projects.tsv`, `last-project.txt` | ayar klasörü | Öğrenme, proje seçimleri, son proje |
| `kamil.log` | önbellek klasörü | Günlük (VS adımları, taramalar, hatalar) |
| `diagnostics.txt`, `vs-connection-report.txt` | önbellek klasörü | Son tanılama raporları |
| `files.bin`, `apps.tsv`, `console-window.txt` | önbellek klasörü | Dosya indeksi, uygulama listesi, konsol penceresi konumu |
| `crash-*.dmp` | önbellek klasörü | Çökme dökümleri (en yeni 5) |

## Geliştirme

### Derleme (Visual Studio 2026)

1. Visual Studio 2026'da **File → Open → Folder** ile repo klasörünü aç.
2. Üst çubuktan preset seç: `x64 Release` (veya `x64 Debug`).
3. **Build → Build All**. Çıktı: `out\build\x64-release\Kamil.exe`

Komut satırından (Developer PowerShell for VS 2026):

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release        # çekirdek birim testleri
```

Tek bir `Kamil.exe` üretilir (statik CRT). VS2022 ile de derlenmesi beklenir ama hedef VS2026'dır.

Linux'ta çekirdek testleri, arama hızı ölçümü ve Windows kaynaklarının MinGW ile derleme denetimi yapılabilir:

```sh
cmake --preset linux-tests && cmake --build --preset linux-tests && ctest --preset linux-tests
out/build/linux-tests/kamil_bench          # tuş başı arama süresi (Release derlemesiyle anlamlı)
cmake --preset mingw-check && cmake --build --preset mingw-check
```

### Proje yapısı

```
src/core/       platform bağımsız çekirdek (bulanık arama, ayarlar, öğrenme, CMake, konsol tamponu) — birim testli
src/platform/   Win32: tray, ikonlar, klasör izleme, klavye kancası, süreçler, VS köprüsü (COM/DTE), çökme dökümü
src/providers/  sonuç kaynakları (uygulamalar, git depoları)
src/ui/         arama penceresi ve Kamil Console (Direct2D)
src/app/        uygulama: ayarların uygulanması, projeler, dosyalar, komutlar, konsol, tanılama
tests/          birim testleri + kamil_bench
tools/          vs-probe.ps1: VS2022/2026 DTE (COM) yeteneklerini ölçer
third_party/    rapidyaml (MIT)
```
