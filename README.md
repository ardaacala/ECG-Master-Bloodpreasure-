# Blood Pressure Wi-Fi + BLE Master — Version 2

Waveshare ESP32-S3 Touch AMOLED 1.43 (466×466) için PlatformIO tabanlı tansiyon
simülatörü/master firmware'i. Ekrandaki kadran, sentetik nabız salınımı, Wi-Fi
slave ve BLE slave aynı planlanmış tetikleme zamanını kullanır.

> Bu depo `v2.0.0` donanım düzenini hedefler. 1.91 inç veya farklı pin dizilimli
> kartlara doğrudan yüklenmemelidir.

## Version 2 özellikleri

- 30 Hz AMOLED arayüzü; UI görevi Core 1'de, radyo ve ADC işleri Core 0'da.
- Gerçek nabız araması kapalı; yalnızca ayarlanabilir sentetik BPM kullanılır.
- Sentetik salınım tepe değeri `0.6 mmHg`; fiziksel LED ve UI LED'iyle aynı anda başlar.
- BPM aralığı 30–240, tekrar tetikleme koruması 200 ms.
- Sentetik nabız yalnızca diyastolik ve sistolik basınç sınırları arasında çalışır.
- Wi-Fi ve BLE slave'lere ortak, ileri tarihli tetikleme zamanı gönderilir.
- Telefonda BLE üzerinden BPM ve basınç aralığı ayarlanabilir.
- Açık/koyu tema ve 2 saniye basılı tutarak ekran açma/kapatma.
- Filtreli batarya voltajı ve lookup-table tabanlı yüzde göstergesi.

## Donanım ve pinler

| İşlev | Pin / bilgi |
|---|---|
| Harici basınç sensörü ADC | GPIO2 |
| Kart üzeri BAT_ADC | GPIO4, kart üzerindeki bölücü oranı ×3 |
| Dokunmatik / QMI8658 I²C SDA | GPIO47 |
| Dokunmatik / QMI8658 I²C SCL | GPIO48 |
| Dokunmatik adresi | `0x38` |
| AMOLED QSPI | CS 9, CLK 10, D0–D3 11–14, RST 21 |
| Seri monitör | 115200 baud, USB CDC |

Aktif topoloji:

- Wi-Fi slave: mantıksal ID 1, varsayılan GPIO2 active-high.
- BLE slave: mantıksal ID 2, reklam adı `LED_NODE_1`, GPIO4 active-low.

GPIO47/48 QMI8658 ile ortak kart içi I²C hattıdır. Firmware yalnızca dokunmatik
denetleyiciye erişir; IMU pinleri veya kayıtları değiştirilmez.

## BLE telefon kontrolü

BLE reklam adı: `BP_TRIGGER_MASTER`

Telefon uygulaması UTF-8 metni `CONTROL_RX_UUID` karakteristiğine yazar:

- `72,130,60`: BPM=72, üst sınır=130 mmHg, alt sınır=60 mmHg.
- `100`: yalnızca BPM'i 100 yapar; mevcut sınırlar korunur.
- `TRIGGER`, `SMALL`, `WIFI`, `BLE`, `SMALL WIFI`, `SMALL BLE`: manuel tetikler.

Sınırlar: BPM 30–240, sistolik 31–300, diyastolik 1–sistolik-1.
UUID'ler [include/protocol.h](include/protocol.h) içindedir. Çalışma sırasında
gelen BLE ayarları bilerek yalnızca RAM'de tutulur; flash yazısının UI'ı
dondurması önlenir.

## Kurulum ve derleme

Önerilen ortam: Python 3.11/3.12 ve PlatformIO Core `6.1.19`.

```powershell
python -m pip install -r requirements.txt
python -m platformio run
```

VS Code PlatformIO eklentisi de doğrudan `platformio.ini` dosyasını kullanabilir.
ESP32 platformu `55.03.311`, kütüphaneler de kesin sürümlere sabitlenmiştir.

Donanıma yüklemeden önce güvenli yapılandırmayı oluşturun:

```powershell
Copy-Item include/secrets.example.h include/secrets.h
```

`include/secrets.h` içindeki `WIFI_AP_PASSWORD` değerini Wi-Fi slave ile aynı
yapın. Bu dosya Git tarafından yok sayılır ve GitHub'a gönderilmez. CI derlemesi
parola gerekmediği için `CHANGE_ME` güvenli varsayılanını kullanır.

Wi-Fi slave için aynı işlemi `slave1_wifi/include/secrets.example.h` dosyasını
`slave1_wifi/include/secrets.h` adına kopyalayarak yapın.

## S3'e yükleme

Önce kartın portunu bulun:

```powershell
python -m platformio device list
python -m platformio run --target upload --upload-port COM3
python -m platformio device monitor --port COM3 --baud 115200
```

`COM3` örnektir. VID:PID `303A:1001` olan ESP32-S3 portu seçilmelidir. CP210x
olarak görünen slave portlarına master firmware'i yüklemeyin. Port görünmüyorsa
önce veri destekli USB kablosunu ve Windows Aygıt Yöneticisi'ni kontrol edin.

## Ekran hareketleri

- Açık temada sağdan sola kaydırma: koyu tema.
- Koyu temada soldan sağa kaydırma: açık tema.
- Ekrana 2 saniye basılı tutma: ekranı aç/kapat.

## Batarya ölçümü

GPIO4, kart üzerindeki BAT_ADC direnç bölücüsünü okur. Her 5 saniyede 32 örnek
ortalaması alınır, ×3 bölücü katsayısı uygulanır ve düşük geçiren filtre
kullanılır. Yüzde doğrusal `map()` ile değil, 3.20–4.20 V Li-Po eğrisi üzerinde
lookup table + interpolasyonla hesaplanır. USB/şarj sırasında değer doğal olarak
şarj devresinden etkilenebilir.

## Proje yapısı

- `src/main.cpp`: ADC, sahte nabız, UI, dokunmatik ve görev dağılımı.
- `src/wifi_master.cpp`: Wi-Fi AP, TCP slave ve saat senkronizasyonu.
- `src/ble_master.cpp`: BLE slave ve telefon GATT kontrolü.
- `include/config.h`: ağ, cihaz kimliği, pin ve zamanlama sabitleri.
- `include/protocol.h`: Wi-Fi/BLE paketleri ve UUID'ler.
- `AGENTS.md` / `CLAUDE.md`: Codex ve Claude için güvenli çalışma kuralları.
- `slave1_wifi`: Wi-Fi slave firmware'i.
- `slave2_ble`: GPIO4 active-low BLE slave firmware'i.

Master ve iki slave GitHub Actions ile her push ve pull request'te derlenir. GitHub
release kaynak kodu içerir; cihaz parolasını firmware içine gömmemek için herkese
açık release'e hazır `.bin` dosyası eklenmez.
