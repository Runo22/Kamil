# Kamil

Windows için offline, geliştirici odaklı, Alfred benzeri başlatıcı. `Alt+Space` ile açılır,
yazdıkça Türkçe karakterleri tanıyan bulanık arama yapar ve seçimlerinden öğrenir.

Tasarım ve yol haritası: [`docs/TASARIM.md`](docs/TASARIM.md)

## Durum: Faz 0 (iskelet)

| Özellik | Durum |
|---|---|
| Tray simgesi, tek örnek, `Alt+Space` (değiştirilebilir) | ✓ |
| Arama penceresi: Direct2D + DirectComposition, Win10'da yuvarlak köşe + gölge, açık/koyu tema, sistem vurgu rengi, DPI duyarlı | ✓ |
| Uygulamalar: Başlat Menüsü + Store/UWP (`shell:AppsFolder`), disk önbelleği ile anında hazır, kendi ikonlarıyla | ✓ |
| Bulanık arama: kısaltma (`vsc` → Visual Studio Code), kelime başı / CamelCase, Türkçe katlama (`calisma` → Çalışma) | ✓ |
| Öğrenme: sık/son kullanılan + "bu yazışta bunu seçtin" (yerel, `usage.tsv`) | ✓ |
| YAML ayarlar: şema tabanlı, doğrulama, satır numaralı hata bildirimi, kaydedince canlı yeniden yükleme, JSON Schema | ✓ |
| Takma adlar, gizlenecek uygulama desenleri, Windows ile başlat | ✓ |
| Ayarlar penceresi, özel komutlar, dosya indeksi, projeler/build/debug | Faz 1–3 |
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
| `↑ ↓`, `Ctrl+J/K`, `PgUp/PgDn`, fare tekerleği | Sonuçlarda gezin |
| `Enter` | Aç |
| `Ctrl+Enter` | Dosya konumunu Gezgin'de göster |
| `Shift+Enter` | Yönetici olarak çalıştır |
| `Alt+1…9` | Görünen n. sonucu aç |
| `Ctrl+C` | (metin seçili değilse) seçili öğenin yolunu kopyala |
| `Esc` | Gizle |

Boş pencerede en sık / son kullandıkların listelenir. `Kamil:` ile başlayan dahili komutlar da aranabilir:
`ayar` → *Kamil: Ayarları düzenle*, `tara` → *Kamil: Uygulamaları yeniden tara*, `çıkış` …

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
  hotkey: Alt+Space
appearance:
  theme: auto          # auto | dark | light
  max_rows: 8
search:
  exclude_apps: ['*uninstall*', '*kaldır*']
  aliases:
    - alias: not defteri
      target: Notepad
```

## Proje yapısı

```
src/core/       platform bağımsız çekirdek (bulanık arama, ayar altyapısı, öğrenme) — birim testli
src/platform/   Win32 yardımcıları (tray, ikonlar, dosya izleme, kabuk işlemleri)
src/providers/  sonuç kaynakları (uygulamalar)
src/ui/         arama penceresi ve Direct2D çizim katmanı
src/app/        uygulama: mesaj döngüsü, ayarların uygulanması
tests/          birim testleri (bağımlılıksız mini çerçeve)
tools/          vs-probe.ps1: VS2022/2026 DTE (COM) yeteneklerini ölçer
third_party/    rapidyaml (MIT, tek başlık)
```

Linux'ta yalnızca çekirdek testleri ve Windows kaynaklarının MinGW ile derleme denetimi yapılabilir:
`cmake --preset linux-tests && cmake --build --preset linux-tests && ctest --preset linux-tests`
