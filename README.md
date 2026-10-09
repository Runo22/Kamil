# Kamil

Windows için offline, geliştirici odaklı başlatıcı. `Alt+Space` ile açılır, yazdıkça Türkçe karakterleri
tanıyan bulanık arama yapar ve seçimlerinden öğrenir. Arayüz **İngilizce ve Türkçe**; varsayılan olarak Windows'un
görüntü dilini izler (`general.language: auto | en | tr`).

Tasarım ve yol haritası: [`docs/TASARIM.md`](docs/TASARIM.md)

## Durum: Faz 0 + git + Faz 1 (build / debug / çalıştır)

| Özellik | Durum |
|---|---|
| Tray simgesi, tek örnek, `Alt+Space` (değiştirilebilir) | ✓ |
| Arama penceresi: Direct2D + DirectComposition, Win10'da yuvarlak köşe + gölge, açık/koyu tema, sistem vurgu rengi, DPI duyarlı | ✓ |
| Uygulamalar: Başlat Menüsü + Store/UWP (`shell:AppsFolder`), disk önbelleği ile anında hazır, kendi ikonlarıyla | ✓ |
| Bulanık arama: `code` → Visual Studio Code (varsayılan takma ad), kelime başı / CamelCase / baş harfler (`ws` → Windows Security), Türkçe katlama (`calisma` → Çalışma) | ✓ |
| Klasör önceliği: seçtiğin klasörlerin altındaki sonuçlar öne (veya eksi değerle arkaya) | ✓ |
| Dosya ve klasör araması: `search.folders` altındaki her şey (kendi indeksi, Everything gerekmez) | ✓ |
| Script'ler: `.bat .cmd .ps1 .py .pyw .sh .exe` — `Enter` çalıştır, `Alt+E` düzenle (VS Code / Not Defteri) | ✓ |
| Öğrenme: sık/son kullanılan + "bu yazışta bunu seçtin" (yerel, `usage.tsv`) | ✓ |
| YAML ayarlar: şema tabanlı, doğrulama, satır numaralı hata bildirimi, kaydedince canlı yeniden yükleme, JSON Schema | ✓ |
| Takma adlar, gizlenecek uygulama desenleri, Windows ile başlat | ✓ |
| Git: proje köklerindeki depolar, dal (anında) + değişiklik / ahead-behind (arka planda) | ✓ |
| Eylem paneli (`Ctrl+K`): VS 2026/2022 (Open Folder), VS Code, Gezgin, terminal, Git Bash, Git GUI — kendi ikonlarıyla | ✓ |
| `Tab` tamamlama, alt bilgi çubuğu (dal, değişiklikler, tuş ipuçları) | ✓ |
| CMake projeleri: preset'ler (`CMakePresets.json`), hedefler (CMake File API), seçimler proje başına hatırlanır | ✓ |
| VS 2026/2022'de Build / Rebuild / Configure — COM (DTE) ile, bitişi izlenir, hata/uyarı sayısı tray'de | ✓ |
| Debug: VS'te derle → programı duraklatılmış başlat → VS debugger'ı bağla → devam | ✓ |
| Çalıştır (ayrı konsol penceresinde), diğer preset'lerin build'lerini ayrı ayrı çalıştırma | ✓ |
| COM port seçimi (dostu ad, VID/PID ile hatırlama), argüman şablonu `--port {com}` | ✓ |
| İki dilli arayüz (İngilizce / Türkçe); dahili komutlar iki dilde de aranır (`settings` = `ayarlar`) | ✓ |
| VS iş kuyruğu: **Kamil: VS jobs** listesi, iptal, sınırlı ve nedenli beklemeler, `kamil.log` | ✓ |
| Konsol penceresi (çıktıları Kamil içinde sekmeli gösterme) | sıradaki |
| Özel komutlar, öğe başına kısayollar, LAN yer imleri | Faz B |
| Ayarlar penceresi | ertelendi (istenince) |
| Windows 11 Acrylic arka plan | sonraki adım (şu an iki sistemde de düz yüzey) |

## Derleme (Visual Studio 2026)

1. Visual Studio 2026'da **File → Open → Folder** ile repo klasörünü açın.
2. Üst çubuktan preset seçin: `x64 Release` (veya `x64 Debug`).
3. **Build → Build All**. Çıktı: `out\build\x64-release\Kamil.exe`

Komut satırından (Developer PowerShell for VS 2026):

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release        # çekirdek birim testleri
```

Tek bir `Kamil.exe` üretilir (statik CRT, ek DLL gerekmez). VS2022 ile de derlenmesi beklenir ama hedef VS2026'dır.

## Kullanım

| Tuş | İşlev |
|---|---|
| `Alt+Space` | Aç / kapat |
| `↑ ↓`, `Ctrl+J`, `PgUp/PgDn`, fare tekerleği | Sonuçlarda gezin |
| `Tab` | Seçili öğenin adını arama kutusuna yaz (tamamla) |
| `Ctrl+K` veya metnin sonunda `→` | Eylem paneli; `Esc`/`←` geri |
| `Alt+D` / `Alt+B` / `Alt+R` (CMake projesi seçiliyken) | Debug / Build / Çalıştır — seçili preset, hedef, port ve argümanlarla |
| `Enter` | Aç |
| `Ctrl+Enter` | Dosya konumunu Gezgin'de göster |
| `Shift+Enter` | Yönetici olarak çalıştır |
| `Alt+1…9` | Görünen n. sonucu aç |
| `Ctrl+C` | (metin seçili değilse) seçili öğenin yolunu kopyala |
| `Esc` | Gizle |

Boş pencerede en sık / son kullandıkların listelenir. `Kamil:` ile başlayan dahili komutlar da aranabilir, iki dilde de:
`settings` / `ayar` → *Kamil: Edit settings*, `rescan` / `tara`, `jobs` / `iptal` → *Kamil: VS jobs*, `log`, `quit` …

## Dosyalar ve script'ler

Kamil, `search.folders` ile verdiğin klasörleri arka planda indeksler (Everything gerekmez; varsayılan: Masaüstü ve
Belgeler). Bu klasörlerdeki dosya ve klasörler aramada çıkar ve `priority` kadar öne alınır:

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

| Dosya | Enter | Alt+E | Ctrl+K |
|---|---|---|---|
| `.bat` `.cmd` | `cmd /K` ile çalıştırır | düzenle | yönetici olarak, konum, terminal, yolu kopyala |
| `.ps1` | `powershell -ExecutionPolicy Bypass -NoExit -File` | düzenle | 〃 |
| `.py` / `.pyw` | `py` / `pyw` (veya `scripts.python`) | düzenle | 〃 |
| `.sh` | Git for Windows `bash.exe` | düzenle | 〃 |
| diğer dosyalar | varsayılan uygulamayla aç | düzenle | birlikte aç…, konum, yolu kopyala |
| klasörler | Gezgin | — | VS Code, Visual Studio (Open Folder), terminal |

`scripts.default_action: edit` ile Enter düzenlemeye, `Alt+R` çalıştırmaya döner. `scripts.keep_console_open: false`
konsolun iş bitince kapanmasını sağlar. Yeni dosyalar, pencereyi açtığında (2 dk'dan eski indekste) veya
"Kamil: Yeniden tara" ile görünür.

## Projeler: build, debug, çalıştır

`dev.project_roots` altındaki git depoları ve CMake projeleri aramada çıkar. Bir CMake projesi seçiliyken:

- `Alt+D` **Debug**: VS'te Build All → başarılıysa program yeni bir konsolda **duraklatılmış** başlatılır → VS debugger'ı
  bağlanır → program devam eder (`main`'deki breakpoint'ler de tutar). Argümanlar ve COM port tamamen Kamil'den gelir.
- `Alt+B` **Build**: VS'e `Build.BuildAll` gönderilir; Kamil bitişi izler, sonucu ("✓ 0 hata, 3 uyarı, 41 sn" veya ilk hata)
  tray'de ve alt bilgi çubuğunda gösterir. Derlemeyi her zaman VS yapar.
- `Alt+R` **Çalıştır**: son derlenen exe'yi argümanlarla ayrı bir konsolda başlatır.
- `Ctrl+K`: preset / hedef / COM port seçimi (`Tab` tamamlar), argüman düzenleme, diğer preset'lerin build'lerini
  ayrı ayrı çalıştırma, Rebuild, CMake configure, önbelleği silip yeniden yapılandırma.

VS seçili klasörü açmamışsa Kamil `devenv "<klasör>"` ile açar ve CMake hazırlığını bekler. Open Folder modunda
VS **kendi seçili preset'ini** derler; Kamil'deki preset farklıysa uyarır.

**İş kuyruğu.** VS işleri sırayla çalışır. Alt bilgi çubuğu çalışan işi, ne beklediğini ("CMake hazırlanıyor",
"VS başka build yapıyor", "VS açılıyor") ve `+N sırada` sayısını gösterir. **Kamil: VS jobs** (veya tray menüsü)
çalışan ve sıradaki işleri listeler; Enter seçili işi iptal eder (çalışan build VS'te de durdurulur). Projenin
`Ctrl+K` panelinde de o projenin işleri en üstte iptal edilebilir. Her bekleme `dev.vs_wait_seconds` (varsayılan
90 sn) ile sınırlıdır; aynı iş ikinci kez sıraya alınmaz.

**Sorun giderme.** Yönetici olarak açılmış bir VS'e normal Kamil erişemez (Windows kısıtı): Kamil bunu tespit edip
söyler, VS'i normal açın ya da Kamil'i de yönetici olarak çalıştırın. Visual Studio bulunamazsa "Visual Studio'da aç"
eylemi nedenini yazar; `dev.devenv_path` ile yolu verin. Her VS adımı `kamil.log`'a yazılır (**Kamil: Open log**).

İlk kullanımda bir kez **Kamil: VS bağlantısını test et** komutunu çalıştırın: açık VS örneklerini, Output bölmelerini
ve VS'teki CMake komutlarının adlarını içeren bir rapor açılır. Configure komutu otomatik bulunamazsa rapordaki adı
`dev.vs_configure_command` ayarına yazın.

## Ayarlar

İlk çalıştırmada açıklamalı bir `settings.yaml` oluşturulur:

| Mod | Konum |
|---|---|
| Normal | `%APPDATA%\Kamil\settings.yaml` (önbellek: `%LOCALAPPDATA%\Kamil\cache`) |
| Taşınabilir | `Kamil.exe` yanında `data\` klasörü veya `portable.txt` dosyası varsa her şey `data\` altında |

Dosya kaydedildiği anda uygulanır. Hatalı değer varsayılanına döner ve tray balonunda satır
numarasıyla bildirilir; YAML sözdizimi bozuksa önceki ayarlar korunur. VS Code + YAML eklentisiyle
açarsanız `schema\settings.json` sayesinde otomatik tamamlama ve hata gösterimi çalışır.

```yaml
general:
  language: auto       # auto | en | tr
  hotkey: Alt+Space
appearance:
  theme: auto          # auto | dark | light
  max_rows: 8
search:
  exclude_apps: ['*uninstall*', '*kaldır*']
dev:
  default_vs: vs2026
  devenv_path: ''                         # boş: vswhere / standart kurulum klasörleri
  vs_wait_seconds: 90                     # VS açılışı / CMake hazırlığı için en uzun bekleme
  project_roots: ['D:\src', 'D:\work']   # git depoları burada aranır
  repo_action: vs                         # depoda Enter: vs | code | explorer | terminal
  terminal: wt                            # wt | cmd | powershell | git-bash
  default_args: '--port {com} --baud 115200'
  build_before_debug: true
  aliases:
    - alias: code
      target: Visual Studio Code
    - alias: not defteri
      target: Notepad
  folder_priority:                        # bu klasörlerin altındaki sonuçlar öne çıkar
    - path: 'D:\src\ana-proje'
      priority: 80                        # -100..100; eksi değer aşağı iter
    - path: 'D:\eski'
      priority: -60
```

## Proje yapısı

```
src/core/       platform bağımsız çekirdek (bulanık arama, ayar altyapısı, öğrenme) — birim testli
src/platform/   Win32 yardımcıları (tray, ikonlar, dosya izleme, kabuk işlemleri)
src/providers/  sonuç kaynakları (uygulamalar, git depoları)
src/ui/         arama penceresi ve Direct2D çizim katmanı
src/app/        uygulama: mesaj döngüsü, ayarların uygulanması
tests/          birim testleri (bağımlılıksız mini çerçeve)
tools/          vs-probe.ps1: VS2022/2026 DTE (COM) yeteneklerini ölçer
third_party/    rapidyaml (MIT, tek başlık)
```

Linux'ta yalnızca çekirdek testleri ve Windows kaynaklarının MinGW ile derleme denetimi yapılabilir:
`cmake --preset linux-tests && cmake --build --preset linux-tests && ctest --preset linux-tests`
