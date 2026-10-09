# Kamil — Windows için Offline, Geliştirici Odaklı Başlatıcı

> Durum: **Taslak v1.2** · Uygulama: **Faz 0 + git + Faz 1 + dosya/script araması kodlandı** (bkz. README) · Hedef: **Windows 10 22H2** (x64), Windows 11'de ek görsel iyileştirmeler · Dil: **C++23** · Projeler: **CMake + Ninja, VS Open Folder**

İnternetsiz bir Windows iş bilgisayarı için; uygulama/dosya/klasör/LAN
kaynaklarını anında bulan; script'leri, özel komutları, süreçleri ve CMake projelerinin
build/debug planlarını tek kısayoldan (`Alt+Space`) yöneten, sürekli arka planda hazır duran bir yardımcı.

### Netleşen kararlar (v0.2 – v0.7)

| Konu | Karar |
|---|---|
| İşletim sistemi | Windows 10 22H2 birincil hedef; Win11'de Acrylic + sistem yuvarlak köşeleri otomatik |
| Dil / derleme | C++23, **VS2026** (MSVC v145) hedef; CMake + Ninja, Open Folder. VS2022 ile derlenmesi garanti değil, en iyi çaba |
| Ayar biçimi | **YAML** (rapidyaml), şema tabanlı ayar altyapısı (§9); Ayarlar penceresi **ertelendi** (istenince) |
| Dosya arama | Kendi indeksimiz birincil; **Everything opsiyonel** (kuruluysa ve açıksa kullanılır); yönetici yetkisi olduğu için **MFT/USN indeksleyici** de seçenek |
| Hariç tutma | Aranmayacak klasör/desenler ayarlardan girilir; her sonuçta "Aramadan çıkar" eylemi |
| Ayarlar | Şimdilik `settings.yaml` (açıklamalı, JSON Schema ile editörde tamamlama); ayrı pencere **kullanıcı tekrar isteyene kadar ertelendi** |
| Arayüz dili | **İngilizce + Türkçe**; varsayılan Windows görüntü dilini izler (`general.language: auto | en | tr`) |
| Projeler | CMake + **Ninja** (tek-config, preset başına bir konfigürasyon); `CMakePresets.json` + CMake File API |
| VS'te açma | **Open Folder** |
| VS sürümü | **Varsayılan VS2026**; proje başına tek sürüm (2022 *veya* 2026), ikisi aynı projede kullanılmaz |
| Derleme | **Kamil derlemez**; configure/build/rebuild/clean COM (DTE) ile **varsayılan VS'e (2022 veya 2026)** gönderilir, VS derler |
| COM port | Exe'ye **komut satırı argümanı** olarak (`--port {com}`) |
| Test | CTest/test koşturma **kapsam dışı** |
| VS entegrasyonu | VS2022 / VS2026, COM (DTE) otomasyonu: build tetikleme, bitişi izleme, debug'a bağlanma |
| Öncelik | **Build / Debug / Çalıştır / COM port** akışı ilk geliştirilecek bölüm |
| Özel komutlar | Parametreli, kısayol atanabilir, zincirlenebilir kullanıcı komutları |
| Kısayol | `Alt+Space` |
| Git | Proje köklerindeki depolar sonuçlarda; dal anında (HEAD okunur), değişiklik/ahead-behind arka planda `git status` ile (§5.10) |
| İkonlar | Her sonuç ve eylem **gerçek uygulama/dosya ikonuyla** gösterilir (VS 2026, VS Code, Git Bash, klasör…); ikon yoksa Segoe MDL2 glifi, o da yoksa baş harf |
| Tamamlama | `Tab` seçili öğenin adını (veya bağlama göre preset/dal adını) arama kutusuna yazar |
| Eylem paneli | `Ctrl+K` veya metnin sonunda `→`; her eylem kendi programının ikonuyla |
| Alt bilgi çubuğu | Pencerenin altında küçük bağlam satırı: git dalı, değişiklikler, preset, çalışan iş + tuş ipuçları |
| Görünüm | Sade, az öğeli, sistem temasına uyumlu |

---

## 1. Hedefler ve Hedef Dışı Konular

| # | Hedef | Ölçülebilir kriter |
|---|-------|--------------------|
| H1 | Kısayola basınca anında açılma | Tuş → görünür ilk kare **< 15 ms** |
| H2 | Her tuş vuruşunda anında sonuç | Yerel sağlayıcılar **p50 < 2 ms, p99 < 8 ms** (200k kayıt) |
| H3 | Soğuk açılış | Önbellekten hazır olma **< 100 ms** |
| H4 | Boştayken iz bırakmama | **%0 CPU**, < 40 MB RAM (Ayarlar/Konsol kapalıyken) |
| H5 | Tamamen offline | Sıfır ağ çağrısı (LAN'daki kullanıcı hedefleri hariç) |
| H6 | Ana süreç yetki istemez | Kamil normal kullanıcı olarak çalışır; yönetici gerektiren parçalar ayrı yardımcıda |
| H7 | Taşınabilir | Tek `.exe` + config klasörü; kurulum zorunlu değil |
| H8 | Öğrenen sıralama | Sık/son seçilenler ve "bu sorguda bunu seçerim" bilgisi üstte |
| H9 | Build → debug akışı tek adım | `proje → plan → Debug` ≤ 3 tuş; son plan hatırlanır |

**Hedef dışı:** müzik kontrolü, web araması/çevrimiçi servisler, bulut senkronu, telemetri, otomatik güncelleme.

---

## 2. Teknoloji Kararları

### 2.1 İki katmanlı arayüz

| Katman | Pencereler | Teknoloji | Neden |
|---|---|---|---|
| **Başlatıcı** | Arama penceresi | Saf Win32 + Direct2D/DirectWrite + DirectComposition, kendi çizimimiz | Maksimum hız, ClearType/Türkçe metin, tam görsel kontrol |
| **Çalışma alanı** | Ayarlar, Konsol (build/çalıştırma çıktıları), Plan düzenleyici | Dear ImGui (D3D11 + FreeType), başlatıcıyla aynı temaya boyanmış | Çok kontrollü formlar hızlı geliştirilir; **sadece açıldığında oluşturulur**, kapatınca bellekten atılır |

Böylece başlatıcı hafif ve hızlı kalır, form ağırlıklı ekranlar ise geliştirme maliyetini patlatmaz.

**Windows 10 22H2 ve 11 farkı:**

| Özellik | Windows 10 22H2 | Windows 11 |
|---|---|---|
| Yuvarlak köşe | DirectComposition ile kendimiz çizeriz (kenar yumuşatmalı, gölgeli) | DWM `DWMWA_WINDOW_CORNER_PREFERENCE` |
| Arka plan | Varsayılan **düz (solid)** yüzey; opsiyonel Acrylic (`SetWindowCompositionAttribute`) | Acrylic (`DWMSBT_TRANSIENTWINDOW`) |
| Yazı tipi | Segoe UI | Segoe UI Variable |
| Sistem ikonları | Segoe MDL2 Assets | Segoe Fluent Icons |

İki sistemde de aynı yerleşim ve renkler; Win11 sadece cam efektini ekler.

### 2.2 Dosya arama katmanları

| Katman | Kaynak | Ne zaman |
|---|---|---|
| **K1 – Kapsamlı indeks (varsayılan)** | `FindFirstFileExW` (`FindExInfoBasic`, `LARGE_FETCH`) + `ReadDirectoryChangesW` | Her zaman: Başlat Menüsü, Masaüstü, proje kökleri, script klasörleri, sık kullanılanlar |
| **K2 – Everything (opsiyonel)** | Everything IPC (1.4 `WM_COPYDATA` / 1.5 SDK3 named pipe) | Ayarlarda açık **ve** Everything çalışıyorsa → `f`/`d` ile tüm disk araması |
| **K3 – MFT + USN (opsiyonel)** | `FSCTL_ENUM_USN_DATA`, `FSCTL_READ_USN_JOURNAL` | Everything yoksa tüm disk için; `KamilIndexer.exe` yardımcısı |
| **K4 – Ağ kökleri** | UNC tarama, TTL'li önbellek | LAN paylaşımları (§6) |

**Yönetici yetkisi kullanımı:** Kamil'in kendisi **yükseltilmiş çalışmaz** (yükseltilmiş bir başlatıcı,
açtığı her şeyi de yönetici olarak açar ve sürükle-bırak bozulur). MFT okuma ve korumalı süreçleri
sonlandırma gibi işler küçük `KamilIndexer.exe` yardımcısına verilir: Görev Zamanlayıcı'da
"en yüksek ayrıcalıklarla, oturum açılışında" görevi olarak çalışır (her seferinde UAC sorulmaz),
Kamil'le named pipe üzerinden konuşur. Ayarlardan tek tuşla kurulur/kaldırılır.

Seçim mantığı (`index.full_disk = "auto"`): K3 kuruluysa K3 → değilse Everything çalışıyorsa K2 → yoksa sadece K1.

### 2.3 Hariç tutulacak yerler

- Ayarlar → **Arama → Hariç tutulanlar** listesi: tam yol (`D:\src\eski\`), klasör adı (`node_modules`),
  glob (`**\build-*\**`, `*.tmp`), uzantı.
- Varsayılan liste: `node_modules .git .vs bin obj out build __pycache__ .venv .cache` (değiştirilebilir).
- Her sonuçta `Tab` → **"Aramadan çıkar"** (öğeyi / klasörünü / uzantısını seçerek).
- Hariç tutma değişince indeks sadece etkilenen kısımda güncellenir, tam tarama gerekmez.

### 2.4 Üçüncü parti (hepsi repoda, offline derlenir)

SQLite (geçmiş/öğrenme), rapidyaml (ayarlar), Dear ImGui (çalışma alanı pencereleri), FreeType
(ImGui metni), xxHash. Derleme: CMake (`CMakePresets.json`, Ninja) + MSVC v145 (VS2026), statik CRT (`/MT`), LTO; çıktı `Kamil.exe` + opsiyonel `KamilIndexer.exe`.

---

## 3. Mimari

```
┌──────────────────────────── Kamil.exe (tray'de yaşar, yükseltilmemiş) ────────────────────────────┐
│                                                                                                    │
│  UI Thread (başlatıcı)        Query Engine                       Worker Pool                       │
│  ┌──────────────┐  sorgu  ┌──────────────────┐  sync <2ms   ┌───────────────────────┐            │
│  │ Hotkey/Tray  │───────▶│ Parser (keyword,  │────────────▶│ apps, index, projects,  │            │
│  │ D2D Renderer │        │ argüman, mod)      │              │ commands, bookmarks,    │            │
│  │ Input/Action │◀───────│ Merger + Ranker    │◀──stream────│ calc / async: everything│            │
│  └──────────────┘ sonuç  └──────────────────┘              │ net, proc, script-filter│            │
│         ▲                         │                          └───────────────────────┘            │
│  ┌──────┴───────┐         ┌───────▼─────────┐   ┌──────────────────────────────────────┐          │
│  │ Icon Service │         │ Learning Store   │   │ Index Service (walker, watcher, mmap   │          │
│  └──────────────┘         │ (SQLite)         │   │ snapshot, Everything köprüsü)          │          │
│                           └─────────────────┘   └──────────────────────────────────────┘          │
│  ┌──────────────────────────────────────────────────────────────────────────────────────┐        │
│  │ Dev Services: Toolchain (vswhere) · CMake (presets, File API) · Device Watcher (COM) │        │
│  │ VS Bridge (DTE 17/18: build tetikle/izle, attach) · Run Manager (çalıştırma sekmesi) │        │
│  └──────────────────────────────────────────────────────────────────────────────────────┘        │
│  ┌──────────────────────────────────┐                                                              │
│  │ Workbench (ImGui, istek üzerine): │◀── Ayarlar · Konsol · Plan düzenleyici                     │
│  └──────────────────────────────────┘                                                              │
└───────────────────────────────────────────────────────┬────────────────────────────────────────────┘
                                                        │ named pipe
                                     ┌──────────────────▼──────────────────┐
                                     │ KamilIndexer.exe (yükseltilmiş, ops.)│  MFT/USN, korumalı süreç işlemleri
                                     └─────────────────────────────────────┘
```

### 3.1 Sağlayıcı (Provider) arayüzü

```cpp
struct Query {
    std::u16string_view raw, folded;  // folded: küçük harf + Türkçe/aksan katlanmış
    std::u16string_view keyword;      // "kill", "b", "f" ... (yoksa boş)
    std::u16string_view rest;
    uint64_t charMask;                // ön filtre için karakter bit maskesi
    uint32_t generation;              // eski sorguların sonuçlarını iptal etmek için
};

class IProvider {
public:
    virtual std::string_view id() const = 0;
    virtual bool isAsync() const { return false; }
    virtual void query(const Query&, ResultSink&, std::stop_token) = 0;
    virtual void actions(const Item&, ActionList&) = 0;   // Tab menüsü
    virtual void execute(const Item&, const Action&, ExecContext&) = 0;
};
```

- **Senkron** sağlayıcılar 2 ms bütçe içinde döner; **asenkron** olanlar sonuçları akıtır, yeni
  sonuçlar 50 ms'lik pencerede birleştirilir ki liste zıplamasın.
- Her sorgu bir `generation` taşır; yeni tuş vuruşu eski asenkron işleri iptal eder.

### 3.2 Hız stratejileri

1. **Sürekli hazır süreç**: pencere, fontlar, fırçalar önceden oluşturulmuş; kısayolda sadece göster + tek kare.
2. **`RegisterHotKey`** — düşük seviye klavye hook'u yok (hızlı ve antivirüs/EDR dostu).
3. **mmap indeks anlık görüntüsü** (`index.bin`): string havuzu + sabit boyutlu kayıtlar + önceden
   hesaplanmış katlanmış adlar ve karakter maskeleri; açılışta map edilir, anında arama.
4. **Ön filtre**: kayıt başına 64-bit karakter maskesi; `(m & q) == q` ile adayların %90+'ı elenir (SIMD).
5. **Artımlı arama**: sorgu öncekinin uzantısıysa sadece önceki aday kümesinde aranır.
6. **Paralel skor** + top-K (K=50) min-heap.
7. **İkonlar asla beklenmez**: önce uzantı ikonu, gerçek ikon arka planda; kalıcı `icons.bin`.
8. **Ağ yollarına UI thread'den dokunulmaz** (§6).
9. **Başlatma işçi thread'de**: pencere hemen gizlenir, `ShellExecuteEx`/`CreateProcess` arkada.
10. **VS örnek önbelleği**: açık VS örnekleri ve açtıkları klasörler ROT değişikliğinde güncellenir;
    build/debug isteğinde arama yapılmaz, doğrudan doğru örneğe komut gider.

### 3.3 Veri dizini

```
%APPDATA%\Kamil\            (taşınabilir modda Kamil.exe yanındaki .\data\)
  settings.yaml             ← genel ayarlar (Ayarlar penceresi de buraya yazar; canlı yeniden yükleme)
  commands.yaml             ← özel komutlar
  bookmarks.yaml            ← sık kullanılanlar / LAN
  projects\*.yaml           ← proje/plan ekleri
  schema\*.json             ← otomatik üretilen JSON Schema
  scripts\                  ← script klasörü (otomatik komut olur)
  cache\index.bin           ← mmap indeks
  cache\icons.bin           ← ikon atlası
  kamil.db                  ← SQLite: geçmiş, öğrenme, son planlar, VS komut eşlemeleri
  logs\runs\*.log           ← çalıştırma çıktıları
```

---

## 4. Akıllı Bulma ve Öğrenme

### 4.1 Eşleştirme
- **Bulanık eşleştirme** (fzf v2 benzeri): kelime başı, `CamelCase`, `_ - . \ /` sonrası, ardışık
  karakter ve ön ek bonusları; **baş harfler**: `ws` → **W**indows **S**ecurity. VS Code için alışkanlık `code`:
  varsayılan takma ad `code → Visual Studio Code`.
- **Türkçe katlama**: `İ/i/I/ı` doğru; `ş→s ğ→g ü→u ö→o ç→c ı→i` → `calisma` = "Çalışma".
- **Takma adlar**: `code → Visual Studio Code` (varsayılan), `not defteri → Notepad`.
- **Klasör önceliği** (`search.folder_priority`): belirtilen klasörlerin altındaki sonuçlara -100..100 puan;
  en uzun eşleşen klasör geçerli (`D:\src\ana` 80, `D:\src` 20, `D:\eski` -60).
- **Yazım hatası toleransı**: ≥4 karakterde az sonuç varsa 1 düzenleme mesafesiyle ikinci tur.
- **Yol modu**: `\`, `C:\`, `\\sunucu\` ile başlayınca klasörde gezinme, `Tab` ile tamamlama.

### 4.2 Sıralama
```
skor = 100 × eşleşme(0..1)
     +  40 × bilgi(sorgu_öneki → öğe)       // "te" yazınca hep Tera Term seçildi
     +  30 × frecency(öğe)                    // log(1 + Σ ziyaret × 0.5^(yaş/14gün))
     +  10 × bağlam(saat, ön plandaki uygulama, aktif proje)
     +       klasör_önceliği(öğenin yolu)          // search.folder_priority, -100..100
     +       tür_önceliği (ayarlanabilir)
Takma ad tam eşleşmesi ve sabitlenenler en üstte.
```

### 4.3 Öğrenme / tahmin (yerel, açıklanabilir)
SQLite'ta seçim olayları `(zaman, sorgu, öğe, sıra, ön_plan_uygulaması)`. Modeller: sorgu→öğe,
frecency, saat/gün histogramı, ardışıklık (Markov: "Build sonrası hep *Çalıştır Debug|x64*").
Boş sorguda **"Muhtemelen şimdi"**: 5 tahmin + son kullanılanlar. `Ctrl+Del` = "bunu önerme".
Ayarlar → Öğrenme sekmesinden görüntülenir/sıfırlanır.

---

## 5. Özellikler (Sağlayıcılar)

### 5.1 Uygulamalar
Başlat Menüsü `.lnk`, Masaüstü, `PATH` exe'leri, UWP (`shell:AppsFolder`), Denetim Masası,
`ms-settings:` sayfaları. Aynı hedefe giden kısayollar birleştirilir.

### 5.2 Dosya ve klasörler

| Eylem | Kısayol |
|---|---|
| Aç (varsayılan) | `Enter` |
| İçeren klasörü göster | `Ctrl+Enter` |
| Yönetici olarak çalıştır | `Shift+Enter` |
| VS Code ile aç | `Alt+C` |
| Visual Studio 2022 / 2026 ile aç (CMake klasörü → Open Folder) | `Alt+V` |
| Burada terminal / VS Developer terminali | `Alt+T` / `Alt+D` |
| Yolu kopyala / UNC olarak kopyala | `Ctrl+C` / `Ctrl+Shift+C` |
| Aramadan çıkar | `Tab` menüsü |
| Özel eylemler (§5.10) | `Tab` menüsü |

"Birlikte aç" kuralları uzantıya/klasör desenine göre (`*.py → code`, `CMakeLists.txt içeren klasör → vs2022`).

### 5.3 Sık kullanılanlar, LAN ve intranet
- Elle tanımlı klasör, UNC yol, intranet URL'leri; durum noktası (erişilebilir / değil).
- Şablonlu bağlantılar: `bug 1234` → `http://tracker.lan/issue/1234`.
- Chrome/Edge yerel `Bookmarks` dosyasından intranet yer imleri.
- Doğrudan yazım: `10.1.2.3`, `\\sunucu\paylasim`, `http://...` → anında "Aç".

### 5.4 Script'ler
`scripts\` ve ek klasörlerdeki `.bat .cmd .ps1 .py .pyw .sh .exe` otomatik komut olur. Dosya başındaki
meta ile başlık, kısayol, çalışma modu, Python yorumlayıcısı belirlenir:

```bat
:: @kamil title: Test cihazına deploy et
:: @kamil hotkey: Ctrl+Alt+D
:: @kamil mode: console        (detached | hidden | console | terminal)
```

### 5.5 Global kısayollar
`Alt+Space` + öğe başına global kısayol (uygulama, klasör, script, özel komut, **plan eylemi**:
ör. `Ctrl+Alt+F5` = aktif projenin aktif planıyla Debug). Çakışmalar Ayarlar'da uyarılır.

### 5.6 Süreç yönetimi
- `kill <ad>`: ikon, PID, RAM, CPU; `Enter` sonlandır, `Shift+Enter` aynı adlı tümü, `Ctrl+Enter` ağaç.
  Yükseltilmiş süreçler KamilIndexer üzerinden (kuruluysa) sonlandırılır.
- `lock <dosya>`: dosyayı kim kilitliyor (Restart Manager) — LNK1168 hatasının ilacı.
- `port 8080`: portu dinleyen süreç. `win <başlık>`: pencereler arası geçiş.

### 5.7 Geliştirici araçları
`= 0x1F << 3` (hex/bin hesap), `err 0x80070005` (HRESULT/Win32/NTSTATUS), `env PATH`, `which cl`,
`vs` (kurulu VS'ler, Developer terminali), `code` (VS Code son çalışma alanları), `git` (depolar),
`py` (yorumlayıcılar/venv'ler), `guid`, `ts`, `b64`, `snip`.

### 5.8 Sistem
Kilitle, uyku, yeniden başlat, kapat (onaylı), geri dönüşüm kutusu, aygıt yöneticisi, hizmetler, olay görüntüleyici.

### 5.9 Özel komutlar

Kullanıcı tanımlı, başlatıcıda normal sonuç gibi görünen komutlar. Ayarlar → **Komutlar** sekmesinden
düzenlenir ("Test et" düğmesiyle), `commands.yaml`'a yazılır.

```yaml
# commands.yaml
commands:
  - name: Logları temizle
    keyword: temizle
    run: del /q "D:\logs\*.log"
    shell: cmd              # cmd | pwsh | bash | python | direct
    mode: console           # detached | hidden | console | terminal
    confirm: true
    hotkey: Ctrl+Alt+L

  - name: Seri monitör
    keyword: mon
    run: D:\tools\putty.exe -serial {com} -sercfg {baud},8,n,1,N
    shell: direct
    params:
      - { name: com,  type: comport }                    # canlı COM listesi
      - { name: baud, type: choice, values: [9600, 115200, 921600], default: 115200, remember: true }

  - name: Release'i test PC'ye gönder
    keyword: gonder
    steps:                  # zincir: biri başarısız olursa durur
      - { plan: Sensör/Saha sürümü, action: build }
      - { run: 'robocopy "{output_dir}" \\testpc\deploy /mir', shell: cmd }
      - { run: 'D:\tools\notify.pyw "Deploy tamam"', shell: python, mode: hidden }

actions:                    # bağlamsal eylem: eşleşen öğelerin Tab menüsüne eklenir
  - name: WinMerge ile karşılaştır
    when: ["*.cpp", "*.h", "*.txt"]
    run: '"C:\Program Files\WinMerge\WinMergeU.exe" "{path}" "{clip}"'
```

> YAML'de Windows yolları için kural: tırnaksız veya **tek tırnak** (`'D:\src'`) kullanılır; çift tırnak
> içinde `\` kaçış karakteri olduğundan Kamil, çift tırnaklı bir değerde şüpheli kaçış (`"D:\new"` → `\n`)
> görürse satır numarasıyla uyarır.

- **Parametre tipleri**: `text`, `choice`, `comport`, `file`, `folder`, `project`, `config`, `plan`.
  Parametreli komut seçilince başlatıcı içinde **adım adım soru** sorulur (Raycast "arguments" gibi),
  `remember: true` son değeri hatırlar. Hızlı yol: `mon COM7 115200` yazıp doğrudan `Enter`.
- **Yer tutucular**: `{q}` `{1}..{9}` `{path}` `{dir}` `{name}` `{clip}` `{com}` `{config}` `{preset}`
  `{output}` `{output_dir}` `{project_dir}` `{build_dir}` `{env:VAR}`.
- Özel komutlar plan adımı (`pre`/`post`) olarak da kullanılabilir.

### 5.10 Git entegrasyonu

**Keşif:** `dev.project_roots` altındaki klasörler arka planda taranır (`dev.scan_depth`, `dev.scan_exclude`);
`.git` klasörü veya worktree/submodule `.git` dosyası (`gitdir: …`) olan her klasör bir **depo öğesi** olur.
Bulunan bir deponun içine inilmez. Tarama uygulama taramasıyla birlikte 30 dakikada bir ve ayar değişince yenilenir.

**Dal bilgisi git.exe olmadan:** `HEAD` dosyası doğrudan okunur (`ref: refs/heads/main` → `main`, kopuk HEAD →
kısa commit). Liste açılır açılmaz her deponun dalı alt satırda görünür.

**Durum (arka planda):** Seçili depo için `git --no-optional-locks status --porcelain=v1 -b` çalıştırılır (3 sn zaman
aşımı, konsol penceresi açılmaz, VS/Git GUI ile `index.lock` yarışı olmaz). Sonuç 5 sn önbelleklenir; sadece pencere
açıkken ve seçili depo için yenilenir. Alt bilgi çubuğunda:

```
⎇ feature/com-port   ● 3 değişiklik   ↑1 ↓2                       Enter Visual Studio · Ctrl+K eylemler · Tab
```

**Depo eylemleri** (`Ctrl+K`), her biri programın kendi ikonuyla; varsayılan `Enter` eylemi `dev.repo_action`:

| Eylem | Program |
|---|---|
| Visual Studio 2026'da aç (Open Folder) | `devenv.exe "<klasör>"` (vswhere ile bulunur; 2022 de kuruluysa ikinci satır) |
| VS Code'da aç | `Code.exe "<klasör>"` |
| Gezgin'de aç | `explorer.exe` |
| Terminalde aç | `dev.terminal`: Windows Terminal (`wt -d`), cmd, PowerShell, Git Bash |
| Git Bash'te aç / Git GUI | Git for Windows kurulumundan |
| Yolu kopyala / Dal adını kopyala | — |

**Sonraki adımlar (Faz 1 içinde):**
- `dal <depo>` / `br`: yerel dallar listesi, `Tab` ile tamamlama, `Enter` = `git switch` (değişiklik varsa uyarı).
- Son commit'ler (`git log -n 20 --oneline`), stash sayısı, değişen dosyalar listesi → dosyayı VS/VS Code'da aç.
- LAN'daki git sunucusu için `fetch`/`pull` eylemleri (çıktı Konsol'da).
- Proje (CMake) öğesi ile depo öğesinin birleşmesi: aynı klasör tek satır, alt bilgide dal + aktif preset.

---

## 6. LAN / Ağ Yolları: Donmayı Önleme

Erişilemeyen bir UNC yol Windows'ta 20–30 sn SMB zaman aşımına yol açar. Kurallar:
1. UI thread ağ yoluna **asla** dokunmaz (ikon, öznitelik, varlık kontrolü dahil).
2. Açmadan önce sunucunun 445 portuna **250 ms** zaman aşımlı TCP denemesi (30 sn önbellek);
   erişilemiyorsa uyarı + "yine de dene".
3. Ağ kökleri ayrı TTL'li önbellekte, arka planda düşük öncelikle taranır.
4. Ağ öğesi ikonları dosyaya dokunmadan uzantıdan alınır.

---

## 7. CMake Projeleri, Planlar, Build/Debug ve COM Port (öncelikli bölüm)

**Çalışma şekli:** VS'te projeler **Open Folder** ile açılıyor, generator **Ninja** (tek-config),
COM port exe'ye **komut satırı argümanı** olarak veriliyor. **Kamil kendisi derlemez**: configure / build /
rebuild / clean komutları COM (DTE) üzerinden **varsayılan Visual Studio'ya (2022 veya 2026)** gönderilir;
derlemeyi VS yapar. Test koşturma (CTest) kapsam dışı.

### 7.1 Kavramlar

- **Proje**: `CMakeLists.txt` + `CMakePresets.json` (+ `CMakeUserPresets.json`) içeren klasör.
  Proje kökleri ayarlardan verilir, projeler **otomatik keşfedilir**; ek ayar gerekirse `projects\*.yaml`.
- **Preset**: configure preset'leri otomatik okunur (`inherits`, `include`, `hidden`, `condition` çözümlenir).
  Ninja tek-config olduğundan **her configure preset'i bir konfigürasyondur** (`x64-debug`, `x64-release`, …).
  Kamil preset'leri sadece **listelemek ve çıktıları bulmak** için okur.
- **Hedef (target)**: çalıştırılabilir hedefler ve çıktı yolları **CMake File API** yanıtından okunur. VS Open
  Folder configure ederken File API'yi zaten kullandığı için yanıt build klasöründe hazırdır
  (`<binaryDir>\.cmake\api\v1\reply`); Kamil ayrıca kendi istemci sorgusunu
  (`query\client-kamil\codemodel-v2`) bırakır ki VS'in bir sonraki configure'unda codemodel kesin üretilsin.
- **Varsayılan VS**: Ayarlar → Araç zincirleri'nde seçilir (`vs2022` | `vs2026`); proje veya plan bazında
  değiştirilebilir. Kurulumlar `vswhere.exe` ile bulunur (17.x = 2022, 18.x = 2026).
- **Plan** = `preset + hedef + VS sürümü + COM port + argüman profili + çalışma dizini + ön/son adımlar`.
  Proje başına **son seçili plan** ve global olarak **aktif proje** hatırlanır.

### 7.2 Proje dosyası (opsiyonel ek ayarlar)

```yaml
# projects\sensor.yaml — çoğu alan otomatik keşfedilir, burası sadece eklemeler içindir
root:   D:\src\SensorUI
name:   Sensör Arayüzü
alias:  [sa, sensor]
vs:     default               # default | vs2022 | vs2026
target: SensorUI              # varsayılan çalıştırılacak hedef

args:                         # komut satırı argüman profilleri
  default: --port {com} --baud 115200
  trace:   --port {com} --baud 115200 --log-level trace
  sim:     --simulate

com:
  prefer:   { vid: "0403", pid: "6001" }   # COM numarası değişse de cihazı bulur
  fallback: COM5

plans:
  - name:   Masa testi
    preset: x64-debug
    args:   trace

  - name:   Saha sürümü
    preset: x64-release
    post:   ["command:Release'i test PC'ye gönder"]
```

Argüman dizesi Windows kurallarına göre (boşluk/tırnak kaçışlarıyla) komut satırına çevrilir;
`{com}` yerine planın port'u (`COM7`) yerleştirilir. Önizleme her zaman satırın alt bilgisinde görünür.

### 7.3 VS köprüsü (COM / DTE)

VS, Running Object Table'a `!VisualStudio.DTE.17.0:<pid>` (2022) / `!VisualStudio.DTE.18.0:<pid>`
(2026) adlarıyla kayıtlıdır. Kamil açık örnekleri tarar ve Open Folder ile açılmış klasörü
(`DTE.Solution.FullName`) proje köküyle eşleştirir.

**Örnek bulma sırası** (bir build/debug isteğinde):
1. Planın VS sürümünde, bu klasörü açmış örnek varsa → o.
2. Yoksa aynı sürümde boşta bir örnek varsa → `DTE.ExecuteCommand("File.OpenFolder", "<kök>")`.
3. Yoksa `devenv.exe "<kök>"` ile yeni örnek açılır; DTE ROT'a kaydolana ve ilk CMake configure
   bitene kadar beklenir (durum başlatıcıda: "VS2022 açılıyor… → CMake hazırlanıyor…").

**Komutlar:**

| Kamil eylemi | VS'e gönderilen |
|---|---|
| Build | `ExecuteCommand("Build.BuildAll")` (hedef seçiliyse ilgili hedefin build komutu) |
| Rebuild / Clean | `Build.RebuildAll` / `Build.CleanAll` |
| Configure | VS'in CMake "Configure Cache" komutu |
| Yeniden configure | VS'in "Delete Cache and Reconfigure" komutu |
| Hataya git | `ItemOperations.OpenFile` + `Edit.GoTo <satır>` |
| Debug durdur | `Debugger.Stop()` |
| VS'i öne getir | `MainWindow.Activate()` |

> CMake'e özgü komut adları (Configure / Delete Cache / preset seçimi) VS sürümüne göre değişebildiği için
> sabit kodlanmaz: Kamil açılışta `DTE.Commands` listesinden bulur, Ayarlar → Araç zincirleri'nde
> elle de eşlenebilir. İlk sürümden önce `tools\vs-probe.ps1` ile iş bilgisayarındaki VS2022/2026'da doğrulanacak.

**Preset uyumu:** Open Folder'da build, VS'te **o an seçili configure preset'i** ile yapılır.
- Kamil seçili preset'i okur (DTE / `.vs` çalışma alanı ayarları) ve başlatıcıda gösterir.
- Plan preset'i farklıysa: VS'in preset seçme komutu bulunduysa önce onu gönderir, bulunamadıysa
  "VS'te aktif preset `x64-debug`, plan `x64-release` istiyor" uyarısı + "VS'teki ile derle" / "iptal".
- VS kapalıyken plan preset'i, VS açıldığında seçili gelecek şekilde çalışma alanı ayarına yazılır.

**Build bitişini izleme** (başlatıcı ve tray'de durum, bitince bildirim):
- `DTE.Events.BuildEvents.OnBuildDone` olayı; Open Folder'da güvenilmezse `SolutionBuild.BuildState`
  yoklaması (250 ms, sadece build sürerken).
- Sonuç: VS **Output → Build** bölmesinin metni okunur, `dosya(satır,sütun): error C2065: …` satırları
  ayrıştırılır → "✓ x64-release — 0 hata, 3 uyarı, 41 sn" veya hata listesi (Enter → VS'te o satır).
- Ek doğrulama: hedef exe'nin değişiklik zamanı build başlangıcından yeni mi.

### 7.3a Araştırma notları ve sağlamlaştırma (v0.8)

`vs-probe` çıktısı yerine kamuya açık bilgilerle ilerlendi; belirsiz noktalar çalışma anında keşfedilir:

| Konu | Bilinen | Kamil'in yaklaşımı |
|---|---|---|
| CMake komut adları (Configure/Generate Cache, Delete Cache and Reconfigure) | Menüde var, DTE adları belgelenmemiş | VS'in `DTE.Commands` tablosunda adında `GenerateCache`/`ConfigureCache`/`DeleteCache` geçen komut aranır, örnek başına önbelleklenir; `dev.vs_configure_command` ile elle verilebilir |
| `Build.BuildAll` | `ExecuteCommand` devre dışı komutu çalıştırmaz; olmayan komut adı da aynı hatayı verir | Önce komutun **var olup olmadığı** `DTE.Commands.Item(ad)` ile denetlenir: yoksa beklemeden hata (Open Folder'da `Build.BuildAll`, çözümde `Build.BuildSolution` denenir). Varsa `IsAvailable` olana kadar `dev.vs_wait_seconds` (vars. 90 sn) beklenir; bekleme nedeni ("CMake hazırlanıyor" / "VS başka build yapıyor") alt bilgide görünür |
| Build bitişi | `SolutionBuild.BuildState` (1/2/3); Open Folder'da güvenilirliği bilinmiyor | Önce BuildState; hiç "sürüyor" görülmezse Build bölmesinin sonundaki "All succeeded/failed" satırı; o da yoksa 15 sn sessizlik |
| Build çıktısı | Output → Build bölmesinin GUID'i `{1BD8A850-02D1-11D1-BEE7-00A0C913D1F8}` (dil bağımsız) | Bölme GUID ile, yoksa adıyla ("Build", "Derleme") bulunur; MSVC/Ninja satırları ayrıştırılır |
| Hangi VS penceresi bu klasörü açmış? | `Solution.FullName` Open Folder'da klasörü vermeyebilir | Sırasıyla: FullName eşleşmesi → Kamil'in açtığı örneğin PID'i → pencere başlığı "Klasör - Microsoft Visual Studio" |
| Duraklatılmış sürece bağlanma | `DebugActiveProcess` duraklatılmış sürece bağlanabilir; ilk iş parçacığı devam edince durulur | `CREATE_SUSPENDED` → `Process2.Attach2("Native")` (olmazsa `Attach`) → `ResumeThread`; bağlanamazsa program yine de devam ettirilir ve uyarılır |
| Meşgul VS (RPC_E_CALL_REJECTED) | COM çağrıları reddedilebilir | Köprü iş parçacığında `IMessageFilter` ile 30 sn'ye kadar otomatik yeniden deneme |
| Yönetici olarak açılmış VS | Yükseltilmemiş Kamil, yükseltilmiş VS'i ROT'ta göremez | `devenv.exe` pencereleri başlıklarıyla taranır; klasörü açan pencere yükseltilmişse yeni VS açmak yerine nedeni söyleyen hata verilir |
| `devenv "<klasör>"` hemen kapanırsa | Eski sürüm 180 sn boşuna bekliyordu | Süreç kapandıysa 5 sn sonra hata; klasör başka bir örnekte açıldıysa o kullanılır; tüm beklemeler `dev.vs_wait_seconds` ile sınırlı |
| İş kuyruğu | İşler tek tek çalışır; önceki iş beklerken sonrakiler görünmeden sırada kalıyordu | **Kamil: VS işleri** listesi (çalışan + sıradaki işler, Enter = iptal), alt bilgide "+N sırada", tray menüsünde iptal; aynı iş ikinci kez sıraya alınmaz; çalışan build iptalinde VS'e `Build.Cancel` gönderilir |
| devenv.exe bulunamadı | vswhere `-utf8` çıktısı BOM ile başlayabiliyor → yol eşleşmiyordu | BOM temizlenir; vswhere yoksa `Program Files\Microsoft Visual Studio\18|2022\<sürüm>` taranır; `dev.devenv_path` ile elle verilebilir. "Visual Studio'da aç" eylemi her zaman görünür, bulunamadıysa nedenini yazar |
| VS'te aktif preset | Saklandığı yer belgelenmemiş | Build klasörleri arasında en son configure edilen (File API yanıtı / CMakeCache zamanı) aktif kabul edilir |

Tanı komutu **Kamil: VS bağlantısını test et**, `vs-probe.ps1`'in yaptığını Kamil içinden yapar ve raporu açar
(devenv'in nasıl bulunduğu, devenv pencereleri ve yönetici durumu, COM ile erişilen örnekler, komut adları).
Ayrı iş parçacığında çalışır: kuyrukta takılı bir iş varken de kullanılabilir. Her adım `kamil.log`'a yazılır
(**Kamil: Günlüğü aç**).

### 7.4 Debug akışı

**Varsayılan: "VS'te derle + başlat + bağlan"**
1. (Plan ayarına göre) önce VS'e build gönderilir ve bitişi beklenir; hata varsa durur.
2. Exe, plan argümanları (`--port COM7 …`) ve çalışma diziniyle **`CREATE_SUSPENDED`** olarak başlatılır.
3. Aynı VS örneğinde DTE `Debugger.LocalProcesses` → PID → `Attach2("Native")`.
4. Ana thread devam ettirilir → `main`'deki breakpoint'ler dahil hepsi tutar. VS öne getirilir.

Hangi exe'nin hangi argüman ve portla debug edileceğini tamamen plan belirler; VS'in başlangıç öğesi
(startup item) seçimine bağlı değildir.

**Alternatif modlar (planda seçilir):**

| Mod | Ne yapar |
|---|---|
| IDE içi F5 | Plan argümanlarını `.vs\launch.vs.json`'daki hedefe yazar + `ExecuteCommand("Debug.Start")` (derlemeyi de VS yapar) |
| `devenv /DebugExe <exe> <args>` | Klasör açmadan exe'yi doğrudan debug eden hızlı yol (derleme yok) |

### 7.5 Proje görünümü

`sa` yazınca (sade görünüm, ikonlar Segoe MDL2/Fluent):

```
  Sensör Arayüzü                 Masa testi · x64-debug · VS2022 (açık) · COM7 FTDI
  ─────────────────────────────────────────────────────────────────────────────
  ⬢  Debug       x64-debug · VS2022         derle → başlat → bağlan          ↵
  ⚒  Build       x64-debug · VS2022         VS'te aktif preset: x64-debug   Alt+B
  ▶  Çalıştır    SensorUI · x64-debug       derlendi 12 dk önce             Alt+R
  ▶  Çalıştır    SensorUI · x64-release     derlendi dün 17:40
  ▶  Çalıştır    SensorUI · x64-relwithdeb  derlenmemiş
  ⚒  Configure   x64-debug · VS2022         CMakeCache 2 sa önce
  ⋯  Plan değiştir · COM port · Rebuild · Clean · VS'te aç · Çıktı klasörü
```

- **Her preset'in çıktısı ayrı satır**: var mı, ne zaman derlendi, sürüm bilgisi.
- **Ayrı ayrı çalıştırma**: `Space` ile birden çok preset işaretlenip birlikte çalıştırılır; her biri
  **Konsol penceresinde ayrı sekmede** (ya da tercihe göre Windows Terminal/cmd sekmesinde).
- **Aynı anda çalışan örnekler ve COM port**: bir port aynı anda tek süreçte açılabildiği için Kamil
  her örneğe **ayrı port** atar (ilk örneğe tercih edilen cihaz, diğerlerine kalan portlar; başlatmadan
  önce düzenlenebilir liste). Yeterli port yoksa uyarır ve kalanları **sırayla** çalıştırmayı önerir.
  Argümanında `{com}` geçmeyen profiller (ör. `sim`) bu kısıta takılmaz.
- Çalışan exe derlemeyi kilitleyecekse build göndermeden önce "çalışan örneği kapat?" (Restart Manager).
- Plan değiştirme `Ctrl+P`; seçim anında kaydedilir.
- **VS sürümü**: bir proje tek bir VS sürümüyle (2022 *veya* 2026) kullanılır; sürüm proje bazında seçilir
  (varsayılan: ayarlardaki varsayılan VS). Sürüm değiştirilirse Kamil, CMake önbelleğinin yeniden
  oluşturulacağını hatırlatır.

**Preset seçimi ve tamamlama:** Plan/preset seçimi başlatıcının içinde, yazarak yapılır:

```
sa preset rel▏          Tab →   sa preset x64-release
  ⚙ x64-release        Configure preset · out\build\x64-release · son build 12 dk önce
  ⚙ x64-relwithdebinfo
```

- `Tab` seçili preset'in tam adını yazar; `Enter` planın preset'ini değiştirir ve kaydeder.
- Aynı mekanizma plan, COM port (`sa com`) ve argüman profili (`sa args`) seçiminde de kullanılır.

**Alt bilgi çubuğunda bağlam ve çalışan iş:**

```
⎇ main   ● 2 değişiklik   ⚙ x64-debug   COM7            Enter Debug (VS2026) · Ctrl+P plan · Ctrl+K
⚒ Build x64-debug  0:41  ▓▓▓▓▓▓░░ 37/120   ⎇ main      Esc gizle (build sürer)
```

Bir build/debug/çalıştırma sürerken alt bilgi çubuğu işin durumunu (tür, preset, süre, Ninja ilerlemesi) dal ve
port bilgisiyle birlikte gösterir; pencere kapatılsa bile iş sürer, tray ipucunda da görünür.

### 7.6 Konsol penceresi (çalıştırmalar)

ImGui, istek üzerine açılır. Derleme çıktısı VS'te kalır; Konsol, Kamil'in **başlattığı exe'ler** içindir:
- Her çalıştırma bir sekme: canlı stdout/stderr, ANSI renkleri, arama, çıkış kodu, süre, kullanılan port.
- Sekme başına: durdur, yeniden başlat (aynı planla), **"VS ile bağlan"** (sonradan attach).
- Ayrıca son build'in özeti (VS Output → Build'den okunan hata/uyarı listesi) ayrı bir sekmede.
- Günlükler `logs\runs\`.

### 7.7 COM port yönetimi

- `SetupDiGetClassDevs(GUID_DEVINTERFACE_COMPORT)` ile **dostu adlar** ("USB Serial Port (COM7) — FTDI FT232R");
  `WM_DEVICECHANGE` ile canlı güncelleme.
- **Cihaz kimliğiyle hatırlama** (VID/PID/seri no): COM numarası değişse de plan doğru cihazı seçer.
- Tek yeni cihaz takılınca otomatik atama + bildirim ("COM9 takıldı → Sensör / Masa testi").
- Portu meşgul eden süreç gösterilir (KamilIndexer ile handle taraması) → tek tuşla kapat.
  Debug/çalıştır öncesi port meşgulse sorulur.
- `com` komutu: liste, PuTTY/Tera Term ile aç (özel komutlarla genişletilebilir).

---

## 8. Görünüm: Sade Tasarım

**İlkeler:** tek sütun, az çizgi, gereksiz süs yok; renk sadece seçim ve eşleşme vurgusunda;
bilgi hiyerarşisi yazı ağırlığı ve gri tonlarla.

```
╭────────────────────────────────────────────────────────────────────────╮
│                                                                        │
│   sa deb▏                                                              │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │ ⬢  Debug — Sensör Arayüzü                                     ↵ │  │
│  │    VS2022 · Debug · --port COM7 --baud 115200                    │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│    ⬢  Debug — Sensör Arayüzü                                   Alt+2   │
│       VS2026 · Debug · yeni örnek                                      │
│    ▶  SensorUI · Debug                                         Alt+3   │
│       D:\src\SensorUI\out\build\x64-debug · 12 dk önce                 │
│    ▢  sa_debug_logs                                            Alt+4   │
│       \\testpc\logs · erişilebilir                                     │
│                                                                        │
│   Masa testi · VS2022 · COM7                          Tab  eylemler    │
╰────────────────────────────────────────────────────────────────────────╯
```

**Görsel ölçüler (96 DPI'da, DPI ile ölçeklenir):**

| Öğe | Değer |
|---|---|
| Pencere | 720 px genişlik, ekran yüksekliğinin %22'si hizasında, sonuç sayısına göre büyür (en çok 8 satır) |
| Köşe / kenar | 10 px yarıçap, 1 px kenar (koyu: beyaz %8, açık: siyah %8), yumuşak gölge |
| Arama alanı | 56 px yükseklik, 20 px yazı, ikon/çerçeve yok |
| Satır | 48 px; 32 px ikon; başlık 14 px, alt satır 12 px ikincil renk |
| Seçim | 6 px yuvarlak dolgu, vurgu rengi %16 opaklık |
| Eşleşen harfler | Vurgu rengi, aynı ağırlık |
| Alt bilgi | 28 px, hafif tonlu şerit: solda bağlam (dal, değişiklikler, preset, iş), sağda tuş ipuçları; `appearance.footer` ile kapatılabilir |
| İkonlar | 32 px, gerçek program/dosya ikonu (shell); yoksa MDL2 glifi; yoksa baş harf rozeti |
| Animasyon | 90 ms solma + 6 px kayma (kapatılabilir); yazmaya ilk karede başlanabilir |

**Renkler:**

| Token | Koyu | Açık |
|---|---|---|
| Yüzey | `#1F1F1F` (Win11: Acrylic, `#202020` %80) | `#F9F9F9` (Win11: Acrylic, `#F3F3F3` %80) |
| Metin | `#F2F2F2` | `#1A1A1A` |
| İkincil metin | `#A0A0A0` | `#5F5F5F` |
| Ayırıcı | `#FFFFFF14` | `#0000000F` |
| Vurgu | Sistem vurgu rengi (ayarlardan sabitlenebilir) | aynı |

Ayarlar ve Konsol pencereleri aynı renk/ölçü tokenlarını kullanır; böylece tek bir ürün gibi görünür.

**Klavye**

| Tuş | İşlev |
|---|---|
| `Alt+Space` | Aç/kapat |
| `↑ ↓`, `Ctrl+J` | Gezin |
| `Enter` / `Ctrl+Enter` / `Shift+Enter` | Varsayılan / ikincil / yönetici |
| `Tab` | Tamamla (seçili öğenin adı / preset / dal) |
| `Ctrl+K`, metnin sonunda `→` | Eylem paneli (`Esc` / `←` geri) |
| `Alt+1…9` | Doğrudan seç |
| `Ctrl+P` | Plan değiştir (proje bağlamında) |
| `Alt+B` / `Alt+D` / `Alt+R` | Aktif plan: Build (VS'te) / Debug / Çalıştır |
| `Space` | (Çoklu seçim) işaretle |
| `↑` (boş sorguda) | Sorgu geçmişi |
| `Ctrl+,` | Ayarlar · `Ctrl+L` Konsol |
| `Esc` | Gizle |

---

## 9. Ayarlar: Altyapı ve Pencere

### 9.1 Biçim: YAML

Tüm ayarlar **YAML** dosyalarındadır (iç içe listeler — komutlar, planlar, sık kullanılanlar — TOML'a
göre çok daha okunaklı). Ayrıştırıcı: **rapidyaml** (tek başlık, çok hızlı, istisnasız).

```
%APPDATA%\Kamil\
  settings.yaml        ← genel ayarlar
  commands.yaml        ← özel komutlar ve bağlamsal eylemler
  bookmarks.yaml       ← sık kullanılanlar, LAN, şablonlu bağlantılar
  projects\*.yaml      ← proje/plan ekleri
  themes\*.yaml        ← temalar
  schema\*.json        ← otomatik üretilen JSON Schema (VS Code'da otomatik tamamlama için)
```

Örnek `settings.yaml`:

```yaml
# yaml-language-server: $schema=./schema/settings.json
general:
  hotkey: Alt+Space
  start_with_windows: true
  portable: false

appearance:
  theme: auto              # auto | dark | light | <tema adı>
  acrylic: auto            # auto | on | off
  max_rows: 8
  animations: true

search:
  roots:
    - { path: D:\src,   depth: 8 }
    - { path: D:\tools, depth: 4 }
  exclude: [node_modules, .git, .vs, out, build, __pycache__, .venv, "**\\build-*\\**", "*.tmp"]
  everything: auto         # auto | on | off
  full_disk: auto          # auto | indexer | everything | off

dev:
  default_vs: vs2026       # vs2022 | vs2026
  debug_mode: build_launch_attach   # build_launch_attach | ide_f5 | debugexe
  build_before_debug: true
  console: kamil           # kamil | wt | cmd
```

### 9.2 Şema tabanlı ayar altyapısı

Her ayar C++'ta **tek bir yerde** tanımlanır; okuma, doğrulama, varsayılan değer, Ayarlar penceresindeki
kontrol, arama, açıklama ve JSON Schema hep bu tanımdan üretilir. Yeni bir ayar eklemek = tek satır.

```cpp
// settings_schema.cpp
inline const SettingDef kSettings[] = {
  { "general.hotkey",       Type::Hotkey, "Alt+Space", "Genel",   "Ana kısayol",
    "Kamil'i açıp kapatan global kısayol." },
  { "appearance.max_rows",  Type::Int{ .min = 4, .max = 16 }, 8, "Görünüm", "Görünen satır sayısı" },
  { "search.exclude",       Type::List<Type::Glob>, kDefaultExcludes, "Arama", "Hariç tutulanlar",
    "Aranmayacak klasör adları, yollar veya desenler.", Apply::ReindexAffected },
  { "dev.default_vs",       Type::Enum{ "vs2022", "vs2026" }, "vs2026", "Geliştirme", "Varsayılan Visual Studio" },
  // ...
};
```

| Özellik | Açıklama |
|---|---|
| **Tipler** | `bool`, `int`/`float` (aralıklı), `string`, `enum`, `path`, `path_list`, `glob_list`, `hotkey`, `color`, `duration` (`1h`, `30s`), `object_list` (komutlar, planlar, yer imleri — alt şemalı) |
| **Katmanlar** | varsayılan → `settings.yaml` → proje (`projects\*.yaml`) → plan. Pencerede her değerin **nereden geldiği** gösterilir, "varsayılana dön" tek tık |
| **Doğrulama** | Yüklemede ve kaydetmede; hatalı dosyada **son geçerli ayar korunur**, hata satır/sütunla tray bildirimi + Ayarlar'da kırmızı satır |
| **Canlı yeniden yükleme** | Dosya değişince (`ReadDirectoryChangesW`) yeniden okunur; elle düzenleme ile pencere iki yönlü senkron |
| **Değişiklik dağıtımı** | Değiştirilemez ayar anlık görüntüsü (`std::shared_ptr<const Settings>`) atomik olarak değiştirilir → arama yolunda kilitsiz okuma. Modüller `subscribe("search.*", cb)` ile sadece ilgili değişikliği alır; her ayarın `Apply` politikası var (anında / indeksi güncelle / kısayolu yeniden kaydet / yeniden başlatma gerekli) |
| **Yorumları koruyarak yazma** | Ayarlar penceresi dosyayı baştan yazmaz: değişen değerin dosyadaki konumu (rapidyaml konum bilgisi) bulunup sadece o metin değiştirilir; ekleme/silme blok bazında yapılır. Elle yazılmış yorumlar ve düzen bozulmaz |
| **Yedek** | Her kaydetmede `settings.yaml.bak1…5` döner yedek; Ayarlar → Tanılama'dan geri yükleme |
| **İçe/dışa aktarma** | Tüm ayarları tek `.zip` veya tek birleşik `.yaml` olarak; başka PC'ye taşıma |
| **JSON Schema** | Şemadan otomatik üretilir; VS Code'da (YAML eklentisiyle) otomatik tamamlama ve hata gösterimi |

### 9.3 Ayarlar penceresi (ertelendi)

> **Ertelendi:** kullanıcı tekrar isteyene kadar yapılmayacak. Aşağıdaki tasarım o zamana kadar referans olarak kalır;
> şimdilik açıklamalı `settings.yaml` + JSON Schema (VS Code'da tamamlama ve hata gösterimi) kullanılır.

Tray menüsü, `Ctrl+,` veya `ayarlar` yazarak açılır. Solda sekmeler, üstte **tüm ayarlarda arama**
(VS Code ayarları gibi: "kısayol" yazınca ilgili tüm ayarlar listelenir), sağda şemadan üretilen form.
Başlatıcıdan da doğrudan gidilebilir: `ayar hariç` → Arama → Hariç tutulanlar.

| Sekme | İçerik |
|---|---|
| **Genel** | Ana kısayol (tuş kaydedici), Windows ile başlat, taşınabilir mod, dil, animasyonlar |
| **Görünüm** | Tema (otomatik/koyu/açık), Acrylic, vurgu rengi, satır sayısı, yazı boyutu, **canlı önizleme** |
| **Arama** | İndeks kökleri (derinlik), **hariç tutulanlar** (yol/ad/glob/uzantı; "test et": bir yol yazınca hariç mi gösterir), Everything, KamilIndexer kur/kaldır, ağ kökleri ve TTL, indeks istatistikleri, "yeniden indeksle" |
| **Sık kullanılanlar** | Klasör / UNC / URL / şablonlu bağlantı listesi, takma adlar, erişilebilirlik testi |
| **Komutlar** | Özel komut ve bağlamsal eylem düzenleyici: liste + ayrıntı formu, parametre düzenleyici, yer tutucu yardımı, **Test et** (çıktı Konsol'da) |
| **Script'ler** | Script klasörleri, uzantı → yorumlayıcı eşlemesi (python/venv, bash) |
| **Kısayollar** | Tüm global kısayolların tek listesi, çakışma uyarısı |
| **Projeler** | Proje kökleri, keşfedilen projeler, plan düzenleyici, argüman profilleri (önizlemeli), COM tercihleri |
| **Geliştirme** | **Varsayılan VS (2026)**, bulunan kurulumlar, açık örnekler, debug modu, build-before-debug, CMake komut eşlemeleri, Konsol tercihi |
| **Öğrenme** | Öğrenilen eşleşmeleri görüntüle/sil, sıfırla, tahminleri aç/kapa |
| **Tanılama** | Performans ölçümleri (açılış, tuş başı gecikme p50/p99), bellek, günlükler, ayar yedekleri |

Altta her zaman: **Dosyada aç** (ilgili YAML'i VS Code'da açar), **Varsayılana dön**, hata/uyarı sayacı.

---

## 10. Güvenlik ve Kurumsal Ortam

- Ağ çağrısı yok; tüm bağımlılıklar repoda, offline derlenir.
- Klavye hook'u, kod enjeksiyonu, sürücü yok.
- Yönetici gerektiren tek parça (KamilIndexer) ayrı, isteğe bağlı ve sadece named pipe üzerinden,
  sadece aynı kullanıcının Kamil sürecinden komut kabul eder (pipe ACL + istemci PID doğrulaması).
- Yıkıcı eylemlerde (kill, kapat, `confirm: true` komutlar) onay; kritik sistem süreçleri gizli.

---

## 11. Yol Haritası (v1.0 — 2026-10-09)

### 11.1 Bugüne kadar yapılanlar

| Alan | Durum |
|---|---|
| Başlatıcı: tray, `Alt+Space`, D2D pencere, tema/vurgu, DPI, ikonlar (halo düzeltmesi dahil) | ✓ |
| Arama: Türkçe katlamalı bulanık eşleştirme, öğrenme (frecency + sorgu→seçim), takma adlar, klasör önceliği | ✓ |
| Ayar altyapısı: şema, YAML doğrulama + satır numaralı hata, canlı yeniden yükleme, JSON Schema | ✓ |
| Uygulamalar (Başlat Menüsü + UWP), dosya/klasör indeksi (`search.folders`), script çalıştır/düzenle | ✓ |
| Git: depo keşfi, dal, değişiklik/ahead-behind, depo eylemleri | ✓ |
| CMake + VS: preset/hedef/argüman/COM seçimi, VS'te build/configure (DTE), debug (başlat+bağlan), çalıştır | ✓ (gerçek VS'te doğrulanacak) |
| İki dilli arayüz (İngilizce / Türkçe, sistem diline göre), komutlar iki dilde de aranır | ✓ |
| VS iş kuyruğu: görünür liste, iptal, sınırlı ve nedenli beklemeler, yönetici VS tespiti, `kamil.log` | ✓ |
| Sağlamlaştırma: minidump + "Kamil: Diagnostics", dosya indeksinin canlı güncellenmesi (`ReadDirectoryChangesW`) + disk önbelleği, `kamil_bench` | ✓ |
| Özel komutlar (`commands:`): `run` komut satırı veya son projede `action`, yer tutucular, global kısayollar (varsayılan `Ctrl+Alt+B` build, `Ctrl+Alt+D` debug) | ✓ |
| Hız: File API önbelleği + arka planda ön okuma (Ctrl+K anında), yazarken daraltan + paralel dosya araması (200 bin dosyada tuş başı ~3–10 ms) | ✓ |
| Eylem paneli, `Tab` tamamlama, alt bilgi çubuğu, iş durumu | ✓ |

### 11.2 Sıradaki fazlar

Öncelik sırası; her satırda **neden** ve kabaca **iş büyüklüğü** (K küçük, O orta, B büyük).

**Faz A — Sağlamlaştırma (önce bu: kod henüz gerçek kullanımda doğrulanmadı)**

| İş | Neden | Büyüklük |
|---|---|---|
| ~~Çökme dökümü + "Kamil: Diagnostics"~~, ~~canlı dosya indeksi + disk önbelleği~~, ~~`kamil_bench`~~ | Yapıldı (v1.2) | — |
| Gerçek kullanım geri bildirimleri: VS köprüsü (komut adları, build bitişi, attach), ikonlar, odak/kısayol | Sürüyor: Build tetikleme doğrulandı | O |

**Faz B — Günlük iş akışı (ilk istekte olan ama henüz olmayanlar)**

| İş | Neden | Büyüklük |
|---|---|---|
| ~~Özel komutlar + global kısayollar~~ | Yapıldı (v1.2) | — |
| LAN / intranet: yer imleri, UNC yolları, şablonlu URL'ler (`bug 1234`), erişilebilirlik testi (SMB donmasını önler) | İlk istek: "offline olsa bile LAN üzerinden klasör ve web siteleri" | O |

**Faz C — Geliştirici araçları**

| İş | Neden | Büyüklük |
|---|---|---|
| Konsol penceresi: çalıştırma/build çıktıları sekmelerde, hata listesi → satıra git (VS / VS Code) | Farklı preset'leri "ayrı ayrı göster" isteğinin tam karşılığı | B |
| Hata kodu (`err 0x80070005`), hex/bin hesap (`= 0x1F<<3`) | C++ geliştirmede sık; küçük iş | K |
| VS: IDE içi F5 modu (`launch.vs.json`), preset değiştirme (bulunabilirse) | Gerçek kullanım geri bildirimine göre | O |

**Faz D — İsteğe bağlı (ihtiyaç doğarsa)**

| İş | Ne zaman değer |
|---|---|
| Everything köprüsü (tüm disk araması) | Everything kurulursa; şu an kurulu değil |
| Python venv keşfi, `py` komutu | Python script'leri çoğalırsa |
| Saat/sıra tabanlı tahmin ("muhtemelen şimdi") | Mevcut öğrenme yetersiz kalırsa |
| Windows 11 Acrylic | Windows 11'e geçilirse |
| Ayarlar penceresi (şemadan üretilen form, §9.3) | **Ertelendi**: kullanıcı tekrar isteyince |
| Süreç araçları: `kill`, `lock <dosya>` (LNK1168), `port 8080` | **Ertelendi**: kullanıcı tekrar isteyince |

### 11.3 Bilinçli olarak yapılmayacaklar

| Konu | Neden |
|---|---|
| Müzik, web araması, bulut senkronu, telemetri, otomatik güncelleme | Offline iş bilgisayarı; istenmedi |
| MFT/USN için yönetici yetkili yardımcı servis | Kapsamlı klasör indeksi yeterince hızlı; yönetici servisi güvenlik/kurulum yükü getirir |
| Pano geçmişi, snippet yöneticisi | Windows'ta `Win+V` var; odak dışı |
| Kurulum sihirbazı / MSI | Tek taşınabilir exe yeterli |
| Kamil'in kendisinin derlemesi | Karar: derlemeyi VS yapar, Kamil tetikler |
| Eklenti pazarı, tema mağazası, yapay zekâ entegrasyonu | Offline ortam ve sadelik hedefiyle çelişir |
| Alfred uyumluluğu (Script Filter eklentileri, workflow içe aktarma) | İstenmedi; özel komutlar ve script'ler aynı ihtiyacı karşılar |

---

## 12. Kalan Açık Sorular

1. Gerçek kullanımda VS köprüsü: "Kamil: VS bağlantısını test et" raporu, Debug/Build ilk deneme sonuçları.
2. Aynı anda birden çok örnek çalıştırırken COM port davranışı (§7.5) uygun mu?
3. Faz B'nin sırası: özel komutlar + kısayollar mı, LAN / yer imleri mi önce? (Ayarlar penceresi ertelendi.)
