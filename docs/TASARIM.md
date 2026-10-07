# Kamil — Windows için Offline, Geliştirici Odaklı Başlatıcı

> Durum: **Taslak v0.3** · Hedef: **Windows 10 22H2** (x64), Windows 11'de ek görsel iyileştirmeler · Dil: **C++23** · Projeler: **CMake + Ninja, VS Open Folder**

Alfred'in iş akışını Windows'a, internetsiz bir iş bilgisayarına taşıyan; uygulama/dosya/klasör/LAN
kaynaklarını anında bulan; script'leri, özel komutları, süreçleri ve CMake projelerinin
build/debug planlarını tek kısayoldan (`Alt+Space`) yöneten, sürekli arka planda hazır duran bir yardımcı.

### Netleşen kararlar (v0.2 – v0.3)

| Konu | Karar |
|---|---|
| İşletim sistemi | Windows 10 22H2 birincil hedef; Win11'de Acrylic + sistem yuvarlak köşeleri otomatik |
| Dil | C++23 (MSVC 17.10+), gerekirse C++20'ye düşülebilir şekilde |
| Dosya arama | Kendi indeksimiz birincil; **Everything opsiyonel** (kuruluysa ve açıksa kullanılır); yönetici yetkisi olduğu için **MFT/USN indeksleyici** de seçenek |
| Hariç tutma | Aranmayacak klasör/desenler ayarlardan girilir; her sonuçta "Aramadan çıkar" eylemi |
| Ayarlar | **Ayrı bir Ayarlar penceresi** (config dosyası yine elle düzenlenebilir) |
| Projeler | CMake + **Ninja** (tek-config, preset başına bir konfigürasyon); `CMakePresets.json` + CMake File API |
| VS'te açma | **Open Folder**; Kamil ve VS aynı build klasörünü ve VS'in paketlediği CMake/Ninja'yı kullanır |
| COM port | Exe'ye **komut satırı argümanı** olarak (`--port {com}`) |
| Test | CTest/test koşturma **kapsam dışı** |
| VS entegrasyonu | VS2022 / VS2026, COM (DTE) otomasyonu ile build / configure / debug |
| Öncelik | **Build / Debug / Çalıştır / COM port** akışı ilk geliştirilecek bölüm |
| Özel komutlar | Parametreli, kısayol atanabilir, zincirlenebilir kullanıcı komutları |
| Kısayol | `Alt+Space` |
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

SQLite (geçmiş/öğrenme), toml++ (config), Dear ImGui (çalışma alanı pencereleri), FreeType
(ImGui metni), xxHash. Derleme: CMake + MSVC, statik CRT (`/MT`), LTO; çıktı `Kamil.exe` + opsiyonel `KamilIndexer.exe`.

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
│  │ Dev Services: Toolchain (vswhere + env önbelleği) · CMake (presets, File API) ·         │        │
│  │ Job Runner (build/run, çoklu sekme)     · VS Bridge (DTE 17/18) · Device Watcher (COM) │        │
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
10. **VS ortam önbelleği**: `VsDevCmd.bat` bir kez çalıştırılıp ortam değişkenleri saklanır (§7.3) →
    build/configure 1–2 sn erken başlar.

### 3.3 Veri dizini

```
%APPDATA%\Kamil\            (taşınabilir modda Kamil.exe yanındaki .\data\)
  config.toml               ← genel ayarlar (Ayarlar penceresi de buraya yazar; canlı yeniden yükleme)
  commands.toml             ← özel komutlar
  projects\*.toml           ← proje/plan tanımları
  scripts\                  ← script klasörü (otomatik komut olur)
  plugins\                  ← Alfred uyumlu Script Filter eklentileri
  cache\index.bin           ← mmap indeks
  cache\icons.bin           ← ikon atlası
  kamil.db                  ← SQLite: geçmiş, öğrenme, son planlar, VS ortam önbelleği
  logs\jobs\*.log           ← build/çalıştırma çıktıları
```

---

## 4. Akıllı Bulma ve Öğrenme

### 4.1 Eşleştirme
- **Bulanık eşleştirme** (fzf v2 benzeri): kelime başı, `CamelCase`, `_ - . \ /` sonrası, ardışık
  karakter ve ön ek bonusları; **kısaltma**: `vsc` → **V**isual **S**tudio **C**ode.
- **Türkçe katlama**: `İ/i/I/ı` doğru; `ş→s ğ→g ü→u ö→o ç→c ı→i` → `calisma` = "Çalışma".
- **Takma adlar**: `"not defteri" = "notepad"`.
- **Yazım hatası toleransı**: ≥4 karakterde az sonuç varsa 1 düzenleme mesafesiyle ikinci tur.
- **Yol modu**: `\`, `C:\`, `\\sunucu\` ile başlayınca klasörde gezinme, `Tab` ile tamamlama.

### 4.2 Sıralama
```
skor = 100 × eşleşme(0..1)
     +  40 × bilgi(sorgu_öneki → öğe)       // "te" yazınca hep Tera Term seçildi
     +  30 × frecency(öğe)                    // log(1 + Σ ziyaret × 0.5^(yaş/14gün))
     +  10 × bağlam(saat, ön plandaki uygulama, aktif proje)
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

### 5.9 Alfred uyumlu eklentiler
`plugins\<ad>\` + herhangi bir script; Alfred **Script Filter JSON** çıktısı okunur, `cache.seconds` ile önbelleklenir.

### 5.10 Özel komutlar

Kullanıcı tanımlı, başlatıcıda normal sonuç gibi görünen komutlar. Ayarlar → **Komutlar** sekmesinden
düzenlenir ("Test et" düğmesiyle), `commands.toml`'a yazılır.

```toml
[[command]]
name    = "Logları temizle"
keyword = "temizle"
run     = 'del /q "D:\logs\*.log"'
shell   = "cmd"            # cmd | pwsh | bash | python | direct
mode    = "console"        # detached | hidden | console | terminal
confirm = true
hotkey  = "Ctrl+Alt+L"

[[command]]
name    = "Seri monitör"
keyword = "mon"
run     = 'D:\tools\putty.exe -serial {com} -sercfg {baud},8,n,1,N'
shell   = "direct"
params  = [
  { name = "com",  type = "comport" },                         # canlı COM listesi
  { name = "baud", type = "choice", values = ["9600", "115200", "921600"], default = "115200", remember = true },
]

[[command]]
name  = "Release'i test PC'ye gönder"
keyword = "gonder"
steps = [                                   # zincir: biri başarısız olursa durur
  { plan = "Sensör/Saha sürümü", action = "build" },
  { run = 'robocopy "{output_dir}" \\testpc\deploy /mir', shell = "cmd" },
  { run = 'D:\tools\notify.pyw "Deploy tamam"', shell = "python", mode = "hidden" },
]

[[action]]                                  # bağlamsal eylem: eşleşen öğelerin Tab menüsüne eklenir
name = "WinMerge ile karşılaştır"
when = "*.cpp;*.h;*.txt"
run  = 'C:\Program Files\WinMerge\WinMergeU.exe "{path}" "{clip}"'
```

- **Parametre tipleri**: `text`, `choice`, `comport`, `file`, `folder`, `project`, `config`, `plan`.
  Parametreli komut seçilince başlatıcı içinde **adım adım soru** sorulur (Raycast "arguments" gibi),
  `remember = true` son değeri hatırlar. Hızlı yol: `mon COM7 115200` yazıp doğrudan `Enter`.
- **Yer tutucular**: `{q}` `{1}..{9}` `{path}` `{dir}` `{name}` `{clip}` `{com}` `{config}` `{preset}`
  `{output}` `{output_dir}` `{project_dir}` `{build_dir}` `{env:VAR}`.
- Özel komutlar plan adımı (`pre`/`post`) olarak da kullanılabilir.

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
COM port exe'ye **komut satırı argümanı** olarak veriliyor. Test koşturma (CTest) kapsam dışı.

### 7.1 Kavramlar

- **Proje**: `CMakeLists.txt` + `CMakePresets.json` (+ `CMakeUserPresets.json`) içeren klasör.
  Proje kökleri ayarlardan verilir, projeler **otomatik keşfedilir**; ek ayar gerekirse `projects\*.toml`.
- **Preset**: configure/build preset'leri otomatik okunur (`inherits`, `include`, `hidden`,
  `condition` çözümlenir). Ninja tek-config olduğundan **her configure preset'i bir konfigürasyondur**
  (`x64-debug`, `x64-release`, …).
- **Hedef (target)**: çalıştırılabilir hedefler ve çıktı yolları **CMake File API** ile kesin olarak bilinir.
  VS Open Folder zaten File API kullandığı için build klasöründe yanıt hazır bulunur; Kamil ayrıca kendi
  istemci sorgusunu (`.cmake/api/v1/query/client-kamil/codemodel-v2`) ekler. Elle yol yazmaya gerek kalmaz.
- **Araç zinciri**: `vs2022` (17.x), `vs2026` (18.x) — `vswhere.exe` ile bulunur.
- **Plan** = `preset + hedef + araç zinciri + COM port + argüman profili + çalışma dizini + ön/son adımlar`.
  Proje başına **son seçili plan** ve global olarak **aktif proje** hatırlanır.

### 7.2 VS Open Folder ile uyum

Kamil ve VS **aynı build klasörünü** (`binaryDir`, ör. `out\build\x64-debug`) paylaşır; böylece Kamil'in
derlediğini VS, VS'in derlediğini Kamil görür, iki kez derleme olmaz. Bunun için:

| Konu | Kural |
|---|---|
| CMake / Ninja sürümü | Seçili VS'in **kendi paketlediği** `cmake.exe` ve `ninja.exe` kullanılır (`Common7\IDE\CommonExtensions\Microsoft\CMake\…`). Farklı CMake sürümü önbelleği bozar / gereksiz yeniden configure yaptırır. |
| Configure argümanları | Preset neyse o; Kamil ek `-D` vermez (VS ile önbellek uyuşmazlığı olmasın). |
| Eşzamanlı build | Aynı klasörde iki Ninja aynı anda çalışmamalı. Kamil build öncesi DTE ile VS'in build durumuna bakar; VS derliyorsa bekler veya uyarır. Kamil derlerken VS'in otomatik configure'u tetiklenirse sıraya alınır. |
| VS2022 ↔ VS2026 | İki sürüm aynı build klasörünü farklı CMake sürümüyle kullanırsa çakışır. Plan araç zincirini değiştirince Kamil uyarır; öneri: sürüm başına ayrı preset (`x64-debug-vs26` gibi, `binaryDir` farklı). |
| Argümanlar | VS içinden F5 de aynı argümanlarla çalışsın diye Kamil isteğe bağlı olarak `.vs\launch.vs.json`'daki ilgili hedefin `args` alanını plan argümanlarıyla günceller. |

### 7.3 Proje dosyası (opsiyonel ek ayarlar)

```toml
# projects\sensor.toml — çoğu alan otomatik keşfedilir, burası sadece eklemeler içindir
root      = 'D:\src\SensorUI'
name      = "Sensör Arayüzü"
alias     = ["sa", "sensor"]
toolchain = "vs2022"                 # vs2022 | vs2026 | auto
target    = "SensorUI"               # varsayılan çalıştırılacak hedef

[args]                               # komut satırı argüman profilleri
default = "--port {com} --baud 115200"
trace   = "--port {com} --baud 115200 --log-level trace"
sim     = "--simulate"

[com]
prefer   = { vid = "0403", pid = "6001" }   # COM numarası değişse de cihazı bulur
fallback = "COM5"

[[plan]]
name   = "Masa testi"
preset = "x64-debug"
args   = "trace"

[[plan]]
name      = "Saha sürümü"
preset    = "x64-release"
toolchain = "vs2026"
post      = ["command:Release'i test PC'ye gönder"]
```

Argüman dizesi Windows kurallarına göre (boşluk/tırnak kaçışlarıyla) komut satırına çevrilir;
`{com}` yerine planın port'u (`COM7`) yerleştirilir. Önizleme her zaman satırın alt bilgisinde görünür.

### 7.4 Araç zinciri ve ortam önbelleği

- `vswhere -all -prerelease -format json` → kurulumlar; `installationVersion` 17.x = 2022, 18.x = 2026.
- Ninja için gerekli MSVC ortamı: `VsDevCmd.bat -arch=amd64 -host_arch=amd64` bir kez çalıştırılır,
  ortam farkı `kamil.db`'de saklanır (anahtar: kurulum yolu + sürüm + mimari). VS güncellenince
  otomatik yenilenir. Sonuç: her build'de 1–2 sn'lik vcvars beklemesi yok.
- Preset'teki `architecture`/`toolset` (`strategy: external`) değerleri ortam seçiminde dikkate alınır
  (VS Open Folder ile aynı davranış).

### 7.5 Proje görünümü

`sa` yazınca (sade görünüm, ikonlar Segoe MDL2/Fluent):

```
  Sensör Arayüzü                       Masa testi · x64-debug · VS2022 · COM7 FTDI
  ─────────────────────────────────────────────────────────────────────────────
  ▶  Çalıştır    SensorUI · x64-debug       derlendi 12 dk önce              ↵
  ▶  Çalıştır    SensorUI · x64-release     derlendi dün 17:40
  ▶  Çalıştır    SensorUI · x64-relwithdeb  derlenmemiş
  ⬢  Debug       VS2022 · x64-debug         açık VS örneğine bağlanır       Alt+D
  ⬢  Debug       VS2026 · x64-debug         yeni örnek açılır
  ⚒  Build       x64-debug                                                  Alt+B
  ⚒  Configure   x64-debug                  CMakeCache 2 sa önce
  ⋯  Plan değiştir · COM port · Tüm preset'leri derle · Çıktı klasörü · VS'te aç
```

- **Her preset'in çıktısı ayrı satır**: var mı, ne zaman derlendi, sürüm bilgisi.
- **Ayrı ayrı çalıştırma**: `Space` ile birden çok preset işaretlenip birlikte çalıştırılır; her biri
  **Konsol penceresinde ayrı sekmede** (ya da tercihe göre Windows Terminal/cmd sekmesinde).
- **Aynı anda çalışan örnekler ve COM port**: bir port aynı anda tek süreçte açılabildiği için Kamil
  her örneğe **ayrı port** atar (ilk örneğe tercih edilen cihaz, diğerlerine kalan portlar; başlatmadan
  önce düzenlenebilir liste). Yeterli port yoksa uyarır ve kalanları **sırayla** çalıştırmayı önerir.
  Argümanında `{com}` geçmeyen profiller (ör. `sim`) bu kısıta takılmaz.
- Çalışan exe build'i kilitliyorsa build öncesi "çalışan örneği kapat?" (Restart Manager).
- Plan değiştirme `Ctrl+P`; seçim anında kaydedilir.

### 7.6 İşler (Job Runner) ve Konsol penceresi

| İş | Komut |
|---|---|
| Configure | `cmake --preset <p>` (VS'in cmake.exe'si, önbellekli MSVC ortamı) |
| Build | `cmake --build --preset <p> [--target <t>]` ya da doğrudan `ninja -C <binaryDir> <t>` |
| Rebuild / Clean | `--clean-first` / `--target clean` |
| Yeniden configure | `CMakeCache.txt` silinip configure (VS'teki "Delete Cache and Reconfigure" eşdeğeri) |
| Çalıştır | `CreateProcess` (plan argümanları, çalışma dizini, ortam) |

**Konsol penceresi** (ImGui, istek üzerine açılır):
- Her iş bir sekme: build ve her çalıştırma. Canlı çıktı, ANSI renkleri, arama, çıkış kodu, süre.
- `dosya(satır,sütun): error C2065: …` satırları **hata listesine** dönüşür → çift tık ile açık VS'te
  (DTE `ItemOperations.OpenFile` + `GotoLine`) veya VS Code'da (`code -g`) ilgili satır.
- Ninja `[37/120]` ilerlemesi sekme başlığında yüzde olarak gösterilir.
- Çalışan süreç için: durdur, yeniden başlat (aynı planla), **"VS ile bağlan"** (attach).
- Bitişte tray bildirimi: "✓ x64-release — 0 hata, 3 uyarı, 41 sn". Günlükler `logs\jobs\`.

### 7.7 Visual Studio köprüsü (VS2022 / VS2026, COM/DTE)

VS, Running Object Table'a `!VisualStudio.DTE.17.0:<pid>` (2022) / `!VisualStudio.DTE.18.0:<pid>`
(2026) adlarıyla kayıtlıdır. Kamil açık örnekleri tarar ve Open Folder ile açılmış klasörü
(`DTE.Solution.FullName`) proje köküyle eşleştirir.

**Debug akışı (varsayılan: "başlat + bağlan"):**
1. (Plan ayarına göre) önce Kamil build eder.
2. Exe, plan argümanları (`--port COM7 …`) ve çalışma diziniyle **`CREATE_SUSPENDED`** olarak başlatılır.
3. Seçilen sürümdeki VS örneği bulunur; yoksa `devenv.exe <proje klasörü>` ile Open Folder olarak açılır
   ve DTE hazır olana kadar beklenir.
4. DTE `Debugger.LocalProcesses` → PID → `Attach2("Native")`.
5. Ana thread devam ettirilir → `main`'deki breakpoint'ler dahil hepsi tutar. VS öne getirilir.

Bu yöntem Open Folder'da VS'in o an seçili preset'inden / başlangıç öğesinden **bağımsızdır**: hangi
exe'nin hangi argümanla debug edileceğini tamamen plan belirler. (Open Folder'da aktif preset'i DTE ile
değiştirmenin güvenilir bir yolu olmadığı için bu kritik.)

**Diğer modlar:**

| Mod | Ne yapar |
|---|---|
| `devenv /DebugExe <exe> <args>` | VS açık değilse en hızlı yol; klasör açmadan exe'yi doğrudan debug eder |
| IDE içi F5 | `launch.vs.json` argümanlarını günceller + DTE `ExecuteCommand("Debug.Start")` (VS'te seçili preset/hedef ile) |
| IDE içi build | DTE `ExecuteCommand("Build.BuildAll")` |
| Hataya git | DTE ile dosya + satır aç |
| Debug'ı durdur / yeniden başlat | DTE `Debugger.Stop()` / akışı tekrarla |

### 7.8 COM port yönetimi

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
| Alt bilgi | 28 px, sadece proje bağlamında veya ipuçları açıksa |
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
| `↑ ↓`, `Ctrl+J/K` | Gezin |
| `Enter` / `Ctrl+Enter` / `Shift+Enter` | Varsayılan / ikincil / yönetici |
| `Tab`, `→` | Eylem paneli |
| `Alt+1…9` | Doğrudan seç |
| `Ctrl+P` | Plan değiştir (proje bağlamında) |
| `Alt+B` / `Alt+D` / `Alt+R` | Aktif plan: Build / Debug / Çalıştır |
| `Space` | (Çoklu seçim) işaretle |
| `↑` (boş sorguda) | Sorgu geçmişi |
| `Ctrl+,` | Ayarlar · `Ctrl+L` Konsol |
| `Esc` | Gizle |

---

## 9. Ayarlar Penceresi

Tray menüsü, `Ctrl+,` veya `ayarlar` yazarak açılır. Sol tarafta sekmeler, sağda form; her değişiklik
anında uygulanır ve ilgili `.toml` dosyasına yazılır (dosyayı elle düzenlemek de serbest, iki yönlü senkron).

| Sekme | İçerik |
|---|---|
| **Genel** | Ana kısayol (tuş kaydedici), Windows ile başlat, taşınabilir mod, dil, animasyonlar |
| **Görünüm** | Tema (otomatik/koyu/açık), Acrylic aç/kapa, vurgu rengi, satır sayısı, yazı boyutu, canlı önizleme |
| **Arama** | İndeks kökleri (derinlik), **hariç tutulanlar** (yol/ad/glob/uzantı), Everything (otomatik/açık/kapalı + durum), KamilIndexer kur/kaldır, ağ kökleri ve TTL, indeks istatistikleri, "yeniden indeksle" |
| **Sık kullanılanlar** | Klasör / UNC / URL / şablonlu bağlantı listesi, takma adlar, erişilebilirlik testi |
| **Komutlar** | Özel komut ve bağlamsal eylem düzenleyici: ad, anahtar kelime, komut, kabuk, mod, parametreler, kısayol, **Test et** |
| **Script'ler** | Script klasörleri, uzantı → yorumlayıcı eşlemesi (python/venv, bash) |
| **Kısayollar** | Tüm global kısayolların tek listesi, çakışma uyarısı |
| **Projeler** | Proje kökleri, keşfedilen projeler, plan düzenleyici, argüman profilleri, COM tercihleri, varsayılan debug modu |
| **Araç zincirleri** | Bulunan VS kurulumları (2022/2026), her birinin CMake/Ninja yolu ve sürümü, ortam önbelleğini yenile |
| **Öğrenme** | Öğrenilen eşleşmeleri görüntüle/sil, sıfırla, tahminleri aç/kapa |
| **Tanılama** | Performans ölçümleri (açılış, tuş başı gecikme p50/p99), bellek, günlükler |

---

## 10. Güvenlik ve Kurumsal Ortam

- Ağ çağrısı yok; tüm bağımlılıklar repoda, offline derlenir.
- Klavye hook'u, kod enjeksiyonu, sürücü yok.
- Yönetici gerektiren tek parça (KamilIndexer) ayrı, isteğe bağlı ve sadece named pipe üzerinden,
  sadece aynı kullanıcının Kamil sürecinden komut kabul eder (pipe ACL + istemci PID doğrulaması).
- Yıkıcı eylemlerde (kill, kapat, `confirm = true` komutlar) onay; kritik sistem süreçleri gizli.

---

## 11. Yol Haritası (önceliğe göre yeniden sıralandı)

| Faz | Kapsam | Çıktı |
|---|---|---|
| **0 – İskelet** | Tray, `Alt+Space`, D2D pencere (Win10 düz + yuvarlak köşe, Win11 Acrylic), uygulama sağlayıcı, bulanık eşleştirme, başlatma, `config.toml` | Kullanılabilir mini başlatıcı |
| **1 – Build / Debug (öncelik)** | vswhere + ortam önbelleği, CMakePresets + File API, proje keşfi, plan modeli + son plan hafızası, Job Runner + Konsol penceresi (sekmeler, hata listesi), konfigürasyon başına çalıştırma, Debug VS2022/VS2026 (başlat+bağlan, DebugExe), `launch.vs.json` senkronu, COM port servisi + çoklu örnekte port atama | Günlük build/debug kullanımı |
| **2 – Ayarlar & Özel komutlar** | Ayarlar penceresi (tüm sekmelerin ilk sürümü), özel komutlar + parametre soruları + zincirler, script klasörleri, global öğe kısayolları, hariç tutma düzenleyici | Kodsuz yapılandırma |
| **3 – Arama genişlemesi** | Dosya indeksi + mmap önbellek + watcher, ikon önbelleği, sık kullanılanlar/LAN/şablonlu bağlantılar, Everything (opsiyonel), tam eylem paneli | Alfred eşdeğeri arama |
| **4 – Akıllanma & araçlar** | Öğrenme/tahmin, kill/lock/port/err/hesap, KamilIndexer (MFT/USN), Alfred eklentileri, tema dosyaları | Tam sürüm |

Not: Faz 1 için gereken minimal ayarlar (proje kökleri, varsayılan VS) başlangıçta `config.toml`'dan
okunur; Faz 2'de Ayarlar penceresine taşınır.

Her fazda `kamil-bench` ile performans hedefleri (§1) ölçülür; gerileme CI'da yakalanır.

---

## 12. Kalan Açık Sorular

1. Aynı anda birden çok örnek çalıştırırken varsayılan davranış (§7.5: her örneğe ayrı port, yetmezse sırayla)
   uygun mu?
2. VS2022 ve VS2026'yı **aynı projede** dönüşümlü kullanıyor musun? Kullanıyorsan sürüm başına ayrı
   preset/build klasörü önerisi (§7.2) senin için kabul edilebilir mi?
