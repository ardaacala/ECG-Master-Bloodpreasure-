# Blood Pressure Wi-Fi + BLE Master — Version 2.1.1

Waveshare ESP32-S3 Touch AMOLED 1.43 (466×466) için PlatformIO tabanlı tansiyon
simülatörü/master firmware'i. Ekrandaki kadran, sentetik nabız salınımı, Wi-Fi
slave ve BLE slave aynı planlanmış tetikleme zamanını kullanır.

> Bu depo `v2.1.1` donanım düzenini hedefler. 1.91 inç veya farklı pin dizilimli
> kartlara doğrudan yüklenmemelidir.

## Version 2.1.1 özellikleri

- 30 Hz AMOLED arayüzü; UI görevi Core 1'de, radyo ve ADC işleri Core 0'da.
- Gerçek nabız araması kapalı; yalnızca ayarlanabilir sentetik BPM kullanılır.
- Sentetik salınım tepe değeri `1.5 mmHg`; fiziksel LED ve UI LED'iyle aynı anda başlar.
- BPM aralığı 30–240, tekrar tetikleme koruması 200 ms.
- Sentetik nabız yalnızca diyastolik ve sistolik basınç sınırları arasında çalışır.
- Basınç sensöründeki süreksiz sıçramaları 5 örnek median ve yaklaşık 20 ms
  süreklilik doğrulamasıyla UI ve trigger sınırlarına ulaşmadan engeller.
- Wi-Fi ve BLE slave'lere ortak, ileri tarihli tetikleme zamanı gönderilir.
- Telefonda BLE üzerinden BPM ve basınç aralığı ayarlanabilir.
- Açık/koyu tema ve 2 saniye basılı tutarak ekran açma/kapatma.
- Filtreli batarya voltajı ve lookup-table tabanlı yüzde göstergesi.
- Parola korumalı Wi-Fi OTA firmware yükleme desteği.
- QMI8658 hareket algılamasıyla 5 dakika hareketsizlikte otomatik düşük güç bekleme.

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

GPIO47/48, dokunmatik ve QMI8658 tarafından paylaşılan kart içi I²C hattıdır.
QMI8658'in kartta bağlı `INT1` çıkışı GPIO8'e gider.

## Harekete duyarlı bekleme modu

QMI8658, normal çalışmada fiziksel hareketi 10 Hz düşük güçlü ivme örnekleriyle
izler; ekran dokunuşu da kullanıcı aktivitesi sayılır. Her fiziksel hareket veya
dokunma beş dakikalık sayacı baştan başlatır. Son aktiviteden itibaren beş dakika
geçerse yeni tetikler engellenir,
ekran uyutulur ve ESP32-S3 deep sleep'e girer; bu durumda iki CPU çekirdeği ile
Wi-Fi/BLE radyo alanları kapalıdır. Yalnızca QMI8658'in 21 Hz düşük güç
ivmeölçeri çalışır. Hareket, QMI8658 `INT1` -> GPIO8 hattıyla kartı uyandırır
ve normal çalışma yeniden başlar. BLE'den girilen BPM/basınç aralığı ve tema
RTC belleğinde tutulduğu için bu uyanmada kaybolmaz.

Jiroskop, QMI8658'in düşük güç ivmeölçer modunda donanım gereği kapalıdır.
Bu, aynı hareketle uyanma işlevini daha az batarya tüketimiyle sağlar. IMU
başlatılamazsa cihazın uyanamayacağı bir duruma girmemesi için otomatik bekleme
devre dışı kalır.

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

Projenin master ve Wi-Fi slave için belgelenmiş varsayılan ağı:

- SSID: `ECG_MASTER`
- Parola: `ecgmaster123`

Bu değerler temiz bir repository derlemesinde doğrudan çalışır. Farklı bir yerel
parola kullanmak için güvenli yapılandırmayı oluşturun:

```powershell
Copy-Item include/secrets.example.h include/secrets.h
```

`include/secrets.h` içindeki `WIFI_AP_PASSWORD` değerini Wi-Fi slave ile aynı
yapın. Bu dosya Git tarafından yok sayılır ve GitHub'a gönderilmez.

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

### Wi-Fi OTA yükleme

OTA destekli firmware ilk kez USB üzerinden yüklenmelidir. Sonraki yüklemelerde
bilgisayarı master'ın `ECG_MASTER` ağına bağlayın. S3 varsayılan olarak
`192.168.4.1:3232` adresinde `bp-trigger-master` adıyla OTA hizmeti verir.
OTA parolası varsayılan olarak yerel ve Git tarafından yok sayılan
`include/secrets.h` içindeki `WIFI_AP_PASSWORD` değeridir. İstenirse aynı dosyada
ayrı bir `OTA_PASSWORD` tanımlanabilir.

PowerShell üzerinden kablosuz yükleme:

```powershell
$env:PLATFORMIO_UPLOAD_FLAGS="--auth=BURAYA_OTA_PAROLASI"
python -m platformio run -e esp32-s3-touch-amoled-1-43-ota -t upload
Remove-Item Env:PLATFORMIO_UPLOAD_FLAGS
```

OTA başladığında yeni trigger üretimi ve otomatik standby geçici olarak durur.
Başarılı aktarım sonunda S3 otomatik yeniden başlar. OTA sırasında bağlantının
kesilmemesi için bilgisayarın `ECG_MASTER` ağından ayrılmaması gerekir.

## Ekran hareketleri

- Açık temada sağdan sola kaydırma: koyu tema.
- Koyu temada soldan sağa kaydırma: açık tema.
- Ekrana 2 saniye basılı tutma: ekranı aç/kapat.

## Batarya ölçümü

GPIO4, kart üzerindeki BAT_ADC direnç bölücüsünü okur. Her ölçüm grubu için 32
örnek 20 ms aralıklarla alınır; böylece basınç sensörü tek seferde bloke edilmez.
×3 bölücü katsayısı ve düşük geçiren filtre uygulanır. Yüzde doğrusal `map()`
ile değil, 3.20–4.20 V Li-Po eğrisi üzerinde
lookup table + interpolasyonla hesaplanır. USB/şarj sırasında değer doğal olarak
şarj devresinden etkilenebilir.

### Feature: Battery level while charging fix

`v2.1.0` sürümünde BAT_ADC voltajındaki ani
yukarı sıçrama doğrudan yüzde artışı kabul edilmez. Kart şemasındaki ETA6098
`ISET` direnci 82 kΩ'dur ve şarj akımını 500 mA'e ayarlar. Yazılım, kablo
takılmadan önceki lookup-table SOC değerini korur; yükseliş üç ardışık ölçümle
doğrulanınca 1500 mAh kapasite, %98 hücre coulombic verimi ve CC/CV son-dolum
yavaşlamasıyla SOC'yi zaman üzerinden ilerletir. Böylece şarj sırasında görülen
4.20 V, gerçek batarya hemen dolmuş gibi yorumlanmaz. USB çıkarıldığında 70 mV veya daha büyük
düşüş hızlı uygulanır ve tekrar batarya-only voltaj eğrisine dönülür. Tahmini
şarj durumu aktifken batarya yüzdesinin yanında `CHG` gösterilir.

Modeldeki `BATTERY_CAPACITY_MAH`, `BATTERY_CHARGE_CURRENT_MA` ve
`BATTERY_CHARGE_COULOMBIC_EFFICIENCY` sabitleri farklı pil veya ölçülmüş şarj süresine göre
kalibre edilebilir. Yeni/sağlıklı 1500 mAh pil varsayımı kullanılır; yaşlanma,
sıcaklık ve gerçek kapasite nedeniyle bu yöntem fuel-gauge entegresi kadar kesin
değildir, fakat şarj gerilimini doğrudan yüzdeye çevirmekten belirgin biçimde
daha gerçekçidir.

Kartın ETA6098 `STAT` çıkışı ESP32 GPIO'suna bağlı olmadığı için yazılım yalnızca
BAT_ADC ile şarj kablosunu kesin olarak algılayamaz. Sistem USB zaten takılıyken
soğuk açılırsa geçmiş bir batarya-only referansı yoktur; bu donanım sınırlaması
ayrı bir VBUS/STAT bağlantısı olmadan tamamen çözülemez.

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
