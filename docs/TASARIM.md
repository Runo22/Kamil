# Kamil — Windows için Offline, Geliştirici Odaklı Başlatıcı

> Durum: **Taslak v0.1** · Hedef platform: Windows 10/11 (x64) · Dil: C++20

Alfred'in iş akışını Windows'a, internetsiz bir iş bilgisayarına taşıyan; uygulama/dosya/klasör/LAN
kaynaklarını anında bulan; script'leri, süreçleri ve kendi C++/Python projelerinin build/debug
planlarını tek kısayoldan yöneten, sürekli arka planda hazır duran bir yardımcı.

---

## 1. Hedefler ve Hedef Dışı Konular

**Hedefler**

| # | Hedef | Ölçülebilir kriter |
|---|-------|--------------------|
| H1 | Kısayola basınca anında açılma | Tuş → görünür ilk kare **< 15 ms** |
| H2 | Her tuş vuruşunda anında sonuç | Yerel sağlayıcılar **p50 < 2 ms, p99 < 8 ms** (200k kayıt) |
| H3 | Soğuk açılış | Önbellekten hazır olma **< 100 ms** |
| H4 | Boştayken iz bırakmama | **%0 CPU**, < 40 MB RAM |
| H5 | Tamamen offline | Sıfır ağ çağrısı (LAN'daki kullanıcı hedefleri hariç) |
| H6 | Yönetici yetkisi gerektirmemek | Tüm temel özellikler normal kullanıcıyla çalışır |
| H7 | Taşınabilir | Tek `.exe` + config klasörü; kurulum zorunlu değil |
| H8 | Öğrenen sıralama | Sık/son seçilenler ve "bu sorguda bunu seçerim" bilgisi üstte |

**Hedef dışı:** müzik kontrolü, web araması/çevrimiçi servisler, bulut senkronu, telemetri, otomatik güncelleme.

---

## 2. Teknoloji Kararları

### 2.1 Dil ve UI

**Karar: C++20 + saf Win32 + Direct2D/DirectWrite + DirectComposition, kendi çizdiğimiz UI.**

| Seçenek | Açılış hızı | Görünüm | Bağımlılık | Not |
|---|---|---|---|---|
| **Win32 + D2D (seçilen)** | En iyi | Tam kontrol, Mica/Acrylic | Yok (OS) | ~1–2 MB tek exe |
| Dear ImGui | Çok iyi | "Araç" gibi durur, IME/DPI zahmetli | Az | Prototip için iyi |
| Qt/QML | Orta | İyi | Ağır, lisans | Offline dağıtım zahmetli |
| WPF/WinUI 3 (C#) | Orta (JIT) | Native | .NET runtime | PowerToys Run yaklaşımı |
| Rust + Slint/egui | İyi | İyi | Az | C++ tercihi nedeniyle ikinci planda |

- Pencere **önceden oluşturulur ve gizli bekler**; kısayolda sadece `ShowWindow` + tek kare çizim.
- Win11: `DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_TRANSIENTWINDOW` (Acrylic), yuvarlak köşe
  `DWMWA_WINDOW_CORNER_PREFERENCE`. Win10: Acrylic blur yedeği, o da yoksa düz renk.
- Per-Monitor DPI v2; pencere imlecin/aktif pencerenin bulunduğu monitörde açılır.
- Tema: sistem açık/koyu modu + vurgu rengini takip eder (`AppsUseLightTheme`, `DwmGetColorizationColor`).

### 2.2 Arama altyapısı: Everything mi, native mi?

Everything'in IPC'si hızlıdır (birkaç ms – onlarca ms), fakat **ana listede her tuş vuruşunda**
harmanlanmış (uygulama + klasör + script + proje) sonuç üretmek ve sıralamayı **kendi öğrenme
modelimizle** yapmak için kendi bellek içi indeksimiz daha hızlı ve daha kontrollüdür.
Bu yüzden **katmanlı** bir yapı:

| Katman | Kaynak | Yetki | Ne zaman |
|---|---|---|---|
| **K1 – Kapsamlı indeks (varsayılan)** | `FindFirstFileExW` (`FindExInfoBasic` + `FIND_FIRST_EX_LARGE_FETCH`) ile tanımlı kökler; `ReadDirectoryChangesW` ile canlı takip | Yok | Her zaman: Başlat Menüsü, Masaüstü, proje kökleri, script klasörleri, sık kullanılanlar |
| **K2 – Everything köprüsü** | Everything IPC (1.4 `WM_COPYDATA` / 1.5 SDK3 named pipe) | Yok | Everything çalışıyorsa tüm disk araması (`f ` modu) |
| **K3 – MFT + USN Journal** | `FSCTL_ENUM_USN_DATA`, `FSCTL_READ_USN_JOURNAL` | Admin | Admin varsa ve Everything yoksa tüm disk; ayrı küçük yükseltilmiş servis |
| **K4 – Ağ kökleri** | UNC tarama, TTL'li önbellek | Yok | LAN paylaşımları (aşağıya bakınız) |

Kapsamlı indeks; `node_modules`, `.git`, `.vs`, `bin`, `obj`, `x64`, `out`, `build`, `__pycache__`, `.venv`
gibi gürültü klasörlerini varsayılan olarak hariç tutar (config ile değiştirilebilir). SSD'de
~100k dosya/sn tarama hızı ile tipik geliştirici kapsamı (200–500k kayıt) birkaç saniyede
**arka planda** yeniden doğrulanır; kullanıcı bu sırada önbellekteki indeksle çalışmaya devam eder.

### 2.3 Üçüncü parti (hepsi repo içinde vendored, offline derlenir)

- **SQLite** (tek dosya) — kullanım geçmişi / öğrenme verisi
- **toml++** — config
- **xxHash** — anahtar/hash
- (opsiyonel) **tinyexpr** veya kendi ifade ayrıştırıcımız — hesap makinesi

Derleme: CMake + MSVC, statik CRT (`/MT`), LTO; çıktı tek `Kamil.exe`.

---

## 3. Mimari

```
┌─────────────────────────── Kamil.exe (tek süreç, tray'de yaşar) ───────────────────────────┐
│                                                                                              │
│  UI Thread                    Query Engine                       Worker Pool                 │
│  ┌──────────────┐  sorgu  ┌──────────────────┐  sync <2ms   ┌──────────────────────┐      │
│  │ Hotkey/Tray  │───────▶│ Parser (keyword,  │────────────▶│ Sync Providers        │      │
│  │ D2D Renderer │        │ arg, mod)          │              │ apps, index, bookmarks│      │
│  │ Input/Action │◀───────│ Merger + Ranker    │◀──stream────│ scripts, projects, calc│      │
│  └──────────────┘ sonuç  │ (generation id)    │  async       ├──────────────────────┤      │
│         ▲                └──────────────────┘              │ Async Providers        │      │
│         │ ikon hazır               │                         │ everything, net, proc, │      │
│  ┌──────┴───────┐         ┌───────▼─────────┐               │ script-filter          │      │
│  │ Icon Service │         │ Learning Store   │               └──────────────────────┘      │
│  │ (STA havuzu) │         │ (SQLite)         │                                              │
│  └──────────────┘         └─────────────────┘   ┌──────────────────────────────────┐      │
│                                                   │ Index Service (FS walker, watcher, │      │
│  ┌───────────────────────────────────────┐      │ mmap snapshot, Everything/MFT)      │      │
│  │ Executor: ShellExecuteEx / CreateProcess│      └──────────────────────────────────┘      │
│  │ (UI'yı asla bloklamaz), Build Runner,  │                                                  │
│  │ VS/DTE köprüsü, Device Watcher (COM)   │                                                  │
│  └───────────────────────────────────────┘                                                  │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
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
    virtual void query(const Query&, ResultSink&, const CancelToken&) = 0;
    virtual void actions(const Item&, ActionList&) = 0;   // Tab menüsü
    virtual void execute(const Item&, const Action&, ExecContext&) = 0;
};
```

- **Senkron** sağlayıcılar UI bütçesi (2 ms) içinde döner; **asenkron** olanlar sonuçları akıtır,
  liste "zıplamasın" diye yeni sonuçlar sadece alta eklenir ya da 50 ms'lik pencerede birleştirilir.
- Her sorgu bir `generation` taşır; yeni tuş vuruşu eski asenkron işleri iptal eder.

### 3.2 Hız stratejileri (kritik bölüm)

1. **Sürekli hazır süreç**: tray'de yaşar, pencere hazır, fontlar/fırçalar önceden oluşturulmuş.
2. **Global kısayol `RegisterHotKey` ile** — düşük seviye klavye hook'u kullanılmaz
   (hem daha hızlı hem de kurumsal antivirüs/EDR'ye takılmaz).
3. **mmap indeks anlık görüntüsü** (`index.bin`): string havuzu + sabit boyutlu kayıt dizisi +
   önceden hesaplanmış katlanmış adlar ve karakter maskeleri. Açılışta dosya map edilir → anında arama.
4. **Ön filtre**: her kayıt için 64-bit karakter varlık maskesi; `(entryMask & qMask) == qMask`
   kontrolü adayların %90+'ını tek AND ile eler (SIMD ile 8'li bloklar).
5. **Artımlı arama**: yeni sorgu bir öncekinin uzantısıysa ("vis" → "visu") sadece önceki aday
   kümesinde aranır.
6. **Paralel skor**: 50k üzeri adayda iş havuzu ile bölütlenmiş skorlama, top-K (K=50) min-heap.
7. **İkonlar asla beklenmez**: önce uzantıya göre jenerik ikon, gerçek ikon arka planda gelir;
   kalıcı ikon önbelleği (`icons.bin`, anahtar: `hash(yol + mtime)`; jenerik türlerde uzantı).
8. **Ağ yollarına UI thread'den asla dokunulmaz** (aşağıda §6).
9. **Başlatma işçi thread'de**: pencere hemen gizlenir, `ShellExecuteEx` arkada çalışır.

### 3.3 Veri dizini

```
%APPDATA%\Kamil\            (veya taşınabilir modda Kamil.exe yanındaki .\data\)
  config.toml               ← kullanıcı ayarları (canlı yeniden yükleme)
  projects\*.toml           ← proje/plan tanımları
  scripts\                  ← script klasörü (otomatik komut olur)
  plugins\                  ← Alfred uyumlu Script Filter eklentileri
  cache\index.bin           ← mmap indeks
  cache\icons.bin           ← ikon atlası
  kamil.db                  ← SQLite: geçmiş, öğrenme, son planlar
  logs\build\*.log          ← build çıktıları
```

---

## 4. Akıllı Bulma ve Öğrenme

### 4.1 Eşleştirme

- **Bulanık eşleştirme** (fzf v2 / Sublime benzeri): alt dizi eşleşmesi + bonuslar
  - kelime başı, `CamelCase` geçişi, `_ - . \ /` sonrası, ardışık karakter, ön ek eşleşmesi
  - **kısaltma**: `vsc` → **V**isual **S**tudio **C**ode, `dm` → **D**evice **M**anager
- **Türkçe duyarlı katlama**: `İ/i/I/ı` doğru eşlenir; `ş→s, ğ→g, ü→u, ö→o, ç→c, ı→i` ile
  `calisma` → `Çalışma Klasörü` bulunur.
- **Takma ad / eş anlamlı**: config'te `"not defteri" = "notepad"`, `"hesap" = "calc"`.
- **Yazım hatası toleransı**: ≥4 karakterli sorguda az sonuç varsa 1 düzenleme mesafesi ile ikinci tur.
- **Yol modu**: `\`, `C:\`, `\\sunucu\` ile başlayınca klasör içinde gezinme, `Tab` ile tamamlama.

### 4.2 Sıralama formülü

```
skor = 100 × eşleşme(0..1)
     +  40 × bilgi(sorgu_öneki → öğe)          // Alfred "knowledge": bu yazışta bunu seçtin
     +  30 × frecency(öğe)                       // log(1 + Σ ziyaret × 0.5^(yaş/14gün))
     +  10 × bağlam(öğe, saat, ön plandaki uygulama)
     +       tür_önceliği(app > proje > klasör > script > dosya)
Takma ad tam eşleşmesi ve sabitlenenler (pin) her zaman en üstte.
```

### 4.3 Öğrenme / tahmin (tamamen yerel, açıklanabilir)

SQLite'ta her seçim için olay kaydı: `(zaman, sorgu, öğe_id, sıra, ön_plan_uygulaması)`.

| Model | Ne öğrenir | Kullanım |
|---|---|---|
| Sorgu→öğe | "`te`" yazınca hep Tera Term seçiliyor | 1–2 harfte doğru öğe en üstte |
| Frecency | Sıklık + yakınlık (14 gün yarı ömür) | Genel sıralama |
| Saat/gün histogramı | 09:00'da hep VPN-dışı klasör + VS açılıyor | Boş sorguda öneri |
| Ardışıklık (Markov) | Build sonrası hep "Release çalıştır" seçiliyor | "Sıradaki muhtemel" önerisi |
| Bağlam | VS ön plandayken build/debug eylemleri | Bağlam bonusu |

Boş sorguda açılan pencere: **"Muhtemelen şimdi"** başlığıyla 5 tahmin + son kullanılanlar.
Her öneri `Ctrl+Del` ile "bunu bir daha önerme" yapılabilir.

---

## 5. Özellikler (Sağlayıcılar)

### 5.1 Uygulamalar
- Başlat Menüsü `.lnk` (kullanıcı + ortak), Masaüstü, `PATH` içindeki exe'ler,
  UWP/Store uygulamaları (`shell:AppsFolder`), Denetim Masası öğeleri, `ms-settings:` sayfaları.
- `.lnk` hedefi çözülür, aynı hedefe giden kopyalar birleştirilir.

### 5.2 Dosya ve klasörler
- K1 kapsamlı indeks + `f <sorgu>` (sadece dosya), `d <sorgu>` (sadece klasör) ile K2/K3 tüm disk.
- Eylem menüsü (`Tab`):

| Eylem | Kısayol |
|---|---|
| Aç (varsayılan) | `Enter` |
| İçeren klasörü göster (seçili) | `Ctrl+Enter` |
| Yönetici olarak çalıştır | `Shift+Enter` |
| VS Code ile aç (`code`/`code -g dosya:satır`) | `Alt+C` |
| Visual Studio 2022 / 2026 ile aç | `Alt+V` |
| Burada terminal / **VS Developer Prompt** | `Alt+T` / `Alt+D` |
| Yolu kopyala / UNC olarak kopyala / dosyayı kopyala | `Ctrl+C` / `Ctrl+Shift+C` |
| Birlikte aç… (config'te tanımlı uygulamalar) | `Tab` menüsü |
| Özellikler | `Alt+Enter` |

- Config'te **"birlikte aç" kuralları**: uzantıya/klasör desenine göre varsayılan uygulama
  (ör. `*.sln → vs2022`, `*.py → code`, `D:\src\* klasörü → code`).

### 5.3 Sık kullanılanlar, LAN ve intranet
- Config'te elle tanımlı klasörler, UNC yollar, intranet URL'leri (ör. `http://10.0.0.15/wiki`).
- **Şablonlu bağlantılar**: `wiki {q}` → `http://wiki.local/search?q={q}`, `bug 1234` → `http://tracker.lan/issue/1234`.
- Chrome/Edge yer imleri yerel `Bookmarks` JSON dosyasından okunur (offline, intranet linkleri için ideal).
- Doğrudan yazım: `10.1.2.3`, `\\sunucu\paylasim`, `http://...` yazılınca anında "Aç" sonucu.

### 5.4 Script'ler ve komutlar
- `scripts\` (ve config'te ekstra klasörler) içindeki `.bat .cmd .ps1 .py .pyw .sh .exe` dosyaları
  otomatik komut olur, kendi ikonlarıyla.
- Argüman: `deploy test 192.168.1.20` → `deploy.bat test 192.168.1.20`.
- Script başına meta (dosyanın başındaki yorum satırlarından veya config'ten):

```bat
:: @kamil title: Test cihazına deploy et
:: @kamil hotkey: Ctrl+Alt+D
:: @kamil mode: output        (detached | hidden | output | terminal)
:: @kamil python: D:\venvs\tools\Scripts\python.exe
```

- `mode=output`: çıktı Kamil içinde canlı panelde gösterilir (stdout/stderr renkli, çıkış kodu).
- `.pyw` konsolsuz, `.py` için `py` launcher veya tanımlı venv; `.sh` için Git Bash/WSL.
- `> komut` ile anlık kabuk komutu: `> ipconfig /all` (çıktı panelde).

### 5.5 Global kısayollar
- Ana kısayol (varsayılan `Alt+Space`, değiştirilebilir) + **öğe başına global kısayol**
  (herhangi bir uygulama/klasör/script/plan için). Çakışma tespit edilip uyarı verilir.

### 5.6 Süreç yönetimi
- `kill <ad>` / `k <ad>`: süreçler ikon, PID, RAM, CPU ile listelenir (`NtQuerySystemInformation`).
  - `Enter` sonlandır, `Shift+Enter` aynı adlı tümünü, `Ctrl+Enter` süreç ağacını sonlandır.
  - Yükseltilmiş süreçler kilit ikonuyla işaretlenir.
- `lock <dosya>`: **dosyayı kim kilitliyor?** (Restart Manager API) — "LNK1168: cannot open
  xxx.exe for writing" hatasının çözümü; tek tuşla kilitleyen süreci sonlandır.
- `port 8080`: portu dinleyen süreç (`GetExtendedTcpTable/UdpTable`).
- `win <başlık>`: açık pencereler arasında geçiş.

### 5.7 Geliştirici araçları
| Komut | İşlev |
|---|---|
| `= 0x1F << 3 \| 0b101` | Hesap makinesi; hex/dec/bin/oct dönüşümü, bit işlemleri, `Enter` ile kopyala |
| `err 0x80070005` / `err 5` | HRESULT / Win32 / NTSTATUS hata metni (`FormatMessage`, offline) |
| `env PATH` | Ortam değişkenleri, PATH girdileri tek tek (bozuk olanlar kırmızı) |
| `which cl` | Komutun PATH'te çözüldüğü yer(ler) |
| `vs` | Kurulu VS sürümleri (vswhere), Developer Prompt, son açılan çözümler |
| `code` | VS Code son çalışma alanları (yerel `storage.json`) |
| `git` | Proje köklerindeki depolar → VS Code / VS / terminal / Git GUI'de aç |
| `py` | Bulunan Python yorumlayıcıları ve venv'ler → REPL, terminal, aktive et |
| `guid`, `ts 1696666666`, `b64 ...` | GUID üret, zaman damgası dönüştür, base64 |
| `snip <ad>` | Metin parçaları (yer tutuculu) → panoya veya yazdır |

### 5.8 Sistem
`kilitle`, `uyku`, `yeniden başlat`, `kapat` (onaylı), `geri dönüşüm kutusunu boşalt`,
`aygıt yöneticisi`, `hizmetler`, `olay görüntüleyici`, `ağ bağlantıları` vb.

### 5.9 Alfred uyumlu eklentiler
`plugins\<ad>\plugin.toml` + herhangi bir script. Script sorguyu argüman alır, **Alfred Script
Filter JSON** formatında çıktı verir (`items[].title/subtitle/arg/icon/valid/mods`) — Alfred
alışkanlıklarını ve mevcut script'leri taşımak kolaylaşır. Sonuçlar `cache.seconds` ile önbelleklenir.

---

## 6. LAN / Ağ Yolları: Donmayı Önleme

Offline/LAN ortamında en büyük gecikme kaynağı, erişilemeyen bir UNC yola dokunulduğunda
Windows'un 20–30 sn SMB zaman aşımıdır. Kurallar:

1. UI thread ağ yoluna **asla** dokunmaz (ikon, öznitelik, varlık kontrolü dahil).
2. Açmadan önce **hızlı erişilebilirlik testi**: sunucunun 445 portuna 250 ms zaman aşımlı TCP
   bağlantı denemesi (sonuç 30 sn önbelleklenir). Erişilemiyorsa: "⚠ sunucu yanıt vermiyor" ve
   "yine de dene" eylemi.
3. Ağ kökleri ayrı TTL'li önbellekte (ör. 1 saat), arka planda ve düşük öncelikle taranır.
4. Ağ öğelerinin ikonları dosyaya dokunmadan, uzantıdan (`SHGFI_USEFILEATTRIBUTES`) alınır.
5. Durum noktası: sık kullanılan sunucuların yanında yeşil/kırmızı nokta.

---

## 7. Projeler, Build Planları, COM Port ve Visual Studio Entegrasyonu

Bu bölüm Kamil'i Alfred'den ayıran ana özellik.

### 7.1 Kavramlar

- **Proje**: bir `.sln`/`.vcxproj` veya `CMakePresets.json`/`CMakeLists.txt` ya da Python projesi.
- **Konfigürasyon**: `Debug|x64`, `Release|x64`, `RelWithDebInfo` … (çözüm/preset'ten **otomatik okunur**).
- **Araç zinciri**: `vs2022` (17.x), `vs2026` (18.x) — `vswhere.exe` ile otomatik bulunur
  (yerel dosya, offline çalışır); MSBuild ve devenv yolları buradan türetilir.
- **Plan**: `konfigürasyon + araç zinciri + COM port + argüman profili + çalışma dizini + ön/son adımlar`.
  Projenin **son seçili planı hatırlanır** ve altbilgide görünür.

### 7.2 Örnek proje tanımı (`projects\sensor.toml`)

```toml
name      = "Sensör Arayüzü"
alias     = ["sa", "sensor"]
solution  = 'D:\src\SensorUI\SensorUI.sln'
toolchain = "vs2022"                       # vs2022 | vs2026 | auto
startup   = "SensorUI"                     # başlangıç projesi
output    = 'D:\src\SensorUI\bin\{platform}\{config}\SensorUI.exe'   # yoksa vcxproj'tan çözülür
workdir   = 'D:\src\SensorUI'

[args]                                     # argüman profilleri
default = "--port {com} --baud 115200"
log     = "--port {com} --baud 115200 --log-level trace"
sim     = "--simulate"

[com]
prefer  = { vid = "0403", pid = "6001" }   # FTDI: COM numarası değişse de cihazı bulur
fallback = "COM5"

[[plan]]
name   = "Masa testi"
config = "Debug|x64"
args   = "log"

[[plan]]
name   = "Saha sürümü"
config = "Release|x64"
toolchain = "vs2026"
pre    = ["scripts/version_bump.py"]
post   = ['xcopy /y "{output}" \\testpc\deploy\']
```

### 7.3 Projede neler görünür?

`sa` yazınca:

```
  ▸ Sensör Arayüzü                          Plan: Masa testi · Debug|x64 · VS2022 · COM7 (FTDI)
    ├ ▶ Çalıştır   SensorUI.exe  Debug|x64          derlendi 12 dk önce        Enter
    ├ ▶ Çalıştır   SensorUI.exe  Release|x64        derlendi dün 17:40
    ├ ▶ Çalıştır   SensorUI.exe  RelWithDebInfo|x64 derlenmemiş (gri)
    ├ 🐞 Debug (VS2022)  ·  🐞 Debug (VS2026)
    ├ 🔨 Build  ·  Rebuild  ·  Clean  ·  CMake Configure
    ├ 🔨 Tüm konfigürasyonları derle (paralel)
    └ ⋯ Plan değiştir · COM port seç · Çıktı klasörü · Çalışan örneği sonlandır
```

- **Her konfigürasyonun çıktısı ayrı öğe** olarak listelenir: exe var mı, ne zaman derlendi,
  sürüm bilgisi, PDB eşleşiyor mu.
- **Ayrı ayrı çalıştırma**: birden fazla konfigürasyonu seçip (`Space` ile işaretle) aynı anda
  çalıştırma; her biri Kamil çıktı panelinde **ayrı sekmede** ya da Windows Terminal'de ayrı
  sekmede (`wt new-tab`) açılır.
- Çalışan exe build'i kilitliyorsa build öncesi "çalışan örneği kapat?" sorulur (Restart Manager).

### 7.4 Build çalıştırıcı

- MSBuild: `MSBuild.exe <sln> /m /nologo /v:minimal /p:Configuration=… /p:Platform=…`;
  CMake: `cmake --preset …` / `cmake --build --preset …` (VS Developer ortamı otomatik kurulur).
- Canlı çıktı paneli; `dosya(satır,sütun): error C2065: …` satırları ayrıştırılıp **hata listesine**
  dönüşür → `Enter` ile VS Code'da (`code -g`) veya açık VS'te ilgili satıra gider.
- Bitişte tray bildirimi (offline çalışır): "✓ Release|x64 — 0 hata, 3 uyarı, 41 sn".
- Build günlükleri `logs\build\` altında saklanır; `last build` ile son çıktı tekrar açılır.

### 7.5 Visual Studio köprüsü (VS2022 / VS2026)

| Durum | Yöntem |
|---|---|
| VS kapalı, debug başlat | `devenv.exe /DebugExe <exe> <args>` veya çözümü aç + `/Command Debug.Start` |
| VS açık, çözüm yüklü | **DTE otomasyonu** (COM, Running Object Table: `VisualStudio.DTE.17.0` = 2022, `VisualStudio.DTE.18.0` = 2026); çözüm yolu eşleşen örnek bulunur |
| ↳ Configure | Aktif konfigürasyonu seç (`SolutionConfigurations.Item(...).Activate()`), başlangıç projesini ayarla, debug argümanlarını (`{com}` yerleştirilmiş) yaz |
| ↳ Build | `SolutionBuild.Build()` — IDE içinde |
| ↳ Debug | `Debugger.Go()` (F5 eşdeğeri) |
| VS kapalıyken argüman | `.vcxproj.user` içindeki `LocalDebuggerCommandArguments` güncellenir |

Kısacası tek akış: **`sa` → COM7 seç → "Debug (VS2022)"** → Kamil açık VS'i bulur, Debug|x64'ü
aktif eder, argümanlara `--port COM7` yazar ve F5'e basar.

### 7.6 COM port yönetimi

- Portlar `SetupDiGetClassDevs(GUID_DEVINTERFACE_COMPORT)` ile **dostu adlarıyla** listelenir
  ("USB Serial Port (COM7) — FTDI FT232R"); `WM_DEVICECHANGE` ile canlı güncellenir.
- **Cihaz kimliğiyle hatırlama** (VID/PID/seri no): Windows COM numarasını değiştirse bile plan
  doğru cihazı seçer.
- Tek yeni cihaz takıldığında otomatik seçim + bildirim ("COM9 takıldı — Sensör planına atandı").
- `com` komutu: port listesi, hangi sürecin portu açık tuttuğu, PuTTY/Tera Term ile hızlı aç.

---

## 8. Görünüm ve Etkileşim

```
╭──────────────────────────────────────────────────────────────────────────────╮
│  🔍  sa deb▏                                                     Kamil  ⌥␣  │
├──────────────────────────────────────────────────────────────────────────────┤
│ ▌🐞  Debug (VS2022) — Sensör Arayüzü                                   ↵    │
│      Debug|x64 · --port COM7 --baud 115200 · açık VS örneği bulundu         │
│  🐞  Debug (VS2026) — Sensör Arayüzü                                  Alt+2  │
│      Debug|x64 · yeni devenv örneği açılacak                                │
│  ▶   SensorUI.exe — Debug|x64                                         Alt+3  │
│      D:\src\SensorUI\bin\x64\Debug · derlendi 12 dk önce                    │
│  📁  sa_debug_logs                                                    Alt+4  │
│      \\testpc\logs\sa_debug_logs · ● erişilebilir                           │
├──────────────────────────────────────────────────────────────────────────────┤
│  Plan: Masa testi · VS2022 · COM7       Tab eylemler · Ctrl+P plan · Esc   │
╰──────────────────────────────────────────────────────────────────────────────╯
```

- Acrylic/Mica arka plan, 8 px yuvarlak köşe, Segoe UI Variable, eşleşen harfler vurgu renginde.
- 80 ms'lik hafif açılma (opacity + 4 px kayma) animasyonu; kapatılabilir (performans modu).
- Sağda Raycast tarzı **eylem paneli** (`Tab` / `→`), altta durum çubuğu (aktif plan, build durumu).
- Tema dosyası (`themes\*.toml`): renkler, satır yüksekliği, yazı tipi, saydamlık.

**Klavye**

| Tuş | İşlev |
|---|---|
| `Alt+Space` | Aç/kapat (değiştirilebilir) |
| `↑ ↓`, `Ctrl+J/K` | Gezin |
| `Enter` / `Ctrl+Enter` / `Shift+Enter` | Varsayılan / ikincil / yönetici |
| `Tab`, `→` | Eylem paneli |
| `Alt+1…9` | Doğrudan seç |
| `Ctrl+P` | Plan değiştir (proje bağlamında) |
| `Space` | (Çoklu seçim modunda) işaretle |
| `↑` (boş sorguda) | Sorgu geçmişi |
| `Ctrl+,` | Ayarlar |
| `Esc` | Gizle |

---

## 9. Örnek `config.toml`

```toml
[general]
hotkey      = "Alt+Space"
theme       = "auto"            # auto | dark | light | <tema adı>
portable    = false
animations  = true

[index]
roots    = ['D:\src', 'D:\tools', '%USERPROFILE%\Documents']
exclude  = ["node_modules", ".git", ".vs", "bin", "obj", "x64", "out", "build", "__pycache__", ".venv"]
max_depth = 8
everything = "auto"             # auto | on | off
network_roots = [ { path = '\\fileserver\proje', ttl = "1h", depth = 3 } ]

[[bookmark]]
title = "Test Sunucusu Logları"
path  = '\\testpc\logs'
alias = ["log"]

[[bookmark]]
title = "Intranet Wiki"
url   = "http://10.0.0.15/wiki"

[[link]]                         # şablonlu bağlantı
keyword = "bug"
url     = "http://tracker.lan/issue/{q}"

[open_with]
"*.sln"          = "vs2022"
"*.py"           = "code"
'D:\src\*\'      = "code"        # klasör deseni

[[hotkey]]
keys   = "Ctrl+Alt+T"
target = 'D:\tools\TeraTerm\ttermpro.exe'

[[alias]]
"not defteri" = "notepad"
```

---

## 10. Güvenlik ve Kurumsal Ortam

- Ağ çağrısı yok; tüm bağımlılıklar repo içinde, offline derlenir.
- Düşük seviye klavye hook'u, kod enjeksiyonu, sürücü yok → EDR dostu.
- Admin gerektiren tek isteğe bağlı parça (MFT okuyucu) ayrı ve kapatılabilir.
- `kill` ve `kapat` gibi yıkıcı eylemlerde sistem süreçleri için onay; kritik süreçler listede gizli.
- Script'ler sadece kullanıcının tanımladığı klasörlerden çalışır.

---

## 11. Yol Haritası

| Faz | Kapsam | Çıktı |
|---|---|---|
| **0 – İskelet** | Tray, global kısayol, D2D pencere, Acrylic, uygulama sağlayıcı, bulanık eşleştirme, başlatma | Kullanılabilir mini başlatıcı |
| **1 – Arama çekirdeği** | Kapsamlı indeks + mmap önbellek + watcher, ikon servisi + önbellek, sık kullanılanlar/LAN, eylem paneli, frecency | Günlük kullanım |
| **2 – Güç araçları** | Script'ler + meta, global öğe kısayolları, çıktı paneli, kill/lock/port, hesap/err/env/which | Geliştirici araç kutusu |
| **3 – Projeler** | vswhere, proje/plan modeli, MSBuild/CMake runner + hata listesi, COM port servisi, DTE köprüsü (2022/2026) | Build/debug akışı |
| **4 – Akıllanma** | Sorgu→öğe bilgisi, saat/ardışıklık tahmini, bağlam bonusu, Everything/MFT katmanları, Alfred Script Filter eklentileri, tema dosyaları, ayarlar UI | Tam sürüm |

Her fazda performans hedefleri (§1) için otomatik benchmark (`kamil-bench`): sentetik 500k kayıtlı
indeks üzerinde tuş başı gecikme p50/p99 ve açılış süresi ölçülür; gerileme CI'da yakalanır.

---

## 12. Açık Sorular

1. Windows 10 mu 11 mi? (Mica/Acrylic ve bazı API'ler)
2. İş bilgisayarında yönetici yetkisi var mı? Everything kurulu/izinli mi?
3. Projeler `.sln/.vcxproj` (MSBuild) mi, CMake mi, ikisi de mi?
4. Exe COM portu argümanla mı alıyor, yoksa config dosyasından mı? (Plan buna göre `{com}`'u yerleştirir)
5. "Plan" yalnızca konfigürasyon+port+argüman mı, yoksa ön/son adımlı (kopyala, deploy) zincirler de gerekli mi?
6. Tercih edilen ana kısayol? (`Alt+Space` PowerToys Run ile çakışabilir)
7. Ağ paylaşımlarının sayısı/büyüklüğü — indekslenmeli mi, yoksa sadece sık kullanılan olarak mı kalsın?
