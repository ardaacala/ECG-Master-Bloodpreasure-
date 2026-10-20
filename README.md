# State based Trigger BLE Master Software — Version 5.0.0 (ekransız)

Ekransız, özel ESP32-S3 PCB için PlatformIO tabanlı tansiyon simülatörü/master
firmware'i. Sentetik nabız, isteğe bağlı yerel nabız LED'i, Wi-Fi slave ve BLE
slave aynı planlanmış tetikleme zamanını kullanır. Durum bilgisi seri port ve
BLE üzerinden okunur.

> Bu branch, Waveshare ESP32-S3 Touch AMOLED 1.43 için yazılmış `v4.1.0`
> sürümünün ekransız türevidir. Ekranlı kart için `main` branch'i ve `v4.1.0`
> etiketini kullanın.

## Version 5.0.0 ekransız master

- AMOLED ekran, 30 Hz UI görevi, framebuffer, tema ve dokunmatik kodu kaldırıldı.
  `GFX Library for Arduino` ve `lvgl` artık bağımlılık değildir.
- Ekrandaki nabız LED'inin yerini isteğe bağlı bir GPIO LED'i alır
  (`PULSE_LED_PIN`). LED, slave'lerin ilk fazıyla aynı planlanmış zamanda
  150 ms yanar.
- Ekrandaki `W:`/`B:` bağlantı şeridi seri durum satırına taşındı.
- GPIO6'daki BLE durum LED'i, BLE slave bağlantısını yanıp sönme hızıyla
  gösterir (bkz. [BLE durum LED'i](#ble-durum-ledi)).
- QMI8658 hareket sensörü kaldırıldı. Otomatik bekleme artık manşet basıncına
  bağlıdır: 30 dakika boyunca 5 mmHg veya altı radyoları kapatır, 5 mmHg üstü
  yeniden açar (bkz. [Basınca bağlı bekleme modu](#basınca-bağlı-bekleme-modu)).
- Batarya göstergesi kartta kullanılan Power-Xtra PX103049 (1500 mAh) hücresinin
  ölçümüne kalibre edildi: şarj/dinlenmiş haritası bu hücrenin 13 ölçüm çifti,
  şarj sırasındaki yüzde ilerleyişi ise aynı koşudan fit edilen CC hızı ve CV
  yavaşlamasıdır (bkz. [Şarj sırasında yüzde ilerleyişi](#şarj-sırasında-yüzde-ilerleyişi)).
- Basınç ölçümü, Wi-Fi/BLE OTA ve Version 3 state trigger sözleşmesi değişmedi;
  slave'ler aynı firmware ile çalışır.

## Version 4.1.0 şarj farkındalıklı batarya göstergesi

- Şarj sırasında gösterilen yüzdeye ölçülmüş bir şarj/dinlenmiş gerilim haritası
  tavan olarak uygulanır. Şarj gerilimi önce dinlenmiş karşılığına çevrilir,
  sonra mevcut 3.20–4.20 V eğrisinden yüzdeye geçilir; sabit akım bölgesinde ham
  eğriye göre 21 puana varan fazla gösterim ortadan kalkar.
- Harita, Power-Xtra PX103049 hücresinden alınan 13 ölçüm çiftidir
  ([`powerxtra-px103049-voltage-map`](https://github.com/ardaacala/powerxtra-px103049-voltage-map))
  ve deponun 10 mV adımlı lookup tablosunu 0.05 mV içinde yeniden üretir.
  Ölçülen 3426–4140 mV aralığının dışına ekstrapolasyon yapılmaz.
- Şarj moduna ikinci bir giriş yolu eklenir: 5 dakikalık pencerelerde arka arkaya
  iki kez en az 20 mV sürekli yükseliş. USB zaten takılıyken açılan kartta
  yakalanacak bir gerilim adımı olmadığı için birincil tespit çalışmıyordu.
- CC/CV zaman modeli ve hızlı USB-çıkarma davranışı korunur; harita yalnızca
  tavan koyar, 4100 mV üzerinde sabit gerilim aşaması başladığı için bırakılır.
- Version 3 state trigger paket sözleşmesi ve slave davranışı değişmez.

## Version 3 state trigger protokolü

- Master artik her sentetik nabizda ag paketi gondermez. BPM veya basinc bolgesi
  degistiginde ve slave yeniden baglandiginda state paketini hemen yollar.
- State paketi alici cihaz ID'si, BPM, `BELOW/INSIDE/ABOVE` basinc bolgesi,
  opsiyonel gecikme, revision ve ortak baslangic zamanini tasir.
- State degismedikce saglik/yeniden toparlanma yayini `1, 2, 4, 8, 10, 10...`
  saniyelik exponansiyel araliklarla surer. State degisince veya reconnect olunca
  zamanlama sifirlanir ve yeni paket yine hemen gonderilir.
- ACK, slave'in revision'i uyguladigini veya zaten ayni state'e sahip oldugunu
  dogrular; ACK alinmasi 10 saniyelik seyrek heartbeat'i durdurmaz.
- Wi-Fi ve BLE slave son state'i NVS flash'ta tutar. Ayni semantik state tekrar
  gelirse flash yeniden yazilmaz; cihaz yeniden acildiginda saklanan state kullanilir.
- Basinc bolgesi `INSIDE` iken slave BPM ritmini yerel olarak uretir. `BELOW` ve
  `ABOVE` durumlarinda cikis kapali kalir. Gelismis slave yazilimi bu uc state'i
  farkli medya veya donut isleyicilerine baglayabilir.
- Periyodik saat senkronizasyonu state paketinden ayri kalir.
- Tek bir senkron alici yeterlidir: Wi-Fi ve BLE state teslimati birbirinden
  bagimsizdir; diger transport veya ikinci bir slave baglantisi beklenmez.

Paket yerlesimi, tekrar/ACK kurallari ve slave gelistirme notlari
[docs/STATE_TRIGGER_PROTOCOL_V3.md](docs/STATE_TRIGGER_PROTOCOL_V3.md) dosyasindadir.

## Temel özellikler

- Ekran yok: basınç 500 Hz örneklenir, sensör görevi ve radyo işleri Core 0'da
  bloklamadan çalışır. Durum seri port ve BLE üzerinden okunur.
- Gerçek nabız araması kapalı; yalnızca ayarlanabilir sentetik BPM kullanılır.
- İsteğe bağlı yerel nabız LED'i, fiziksel slave çıkışlarıyla aynı planlanmış
  zamanda yanar.
- BPM aralığı 30–240, tekrar tetikleme koruması 200 ms.
- Sentetik nabız yalnızca diyastolik ve sistolik basınç sınırları arasında çalışır.
- Basınç sensöründeki süreksiz sıçramaları 5 örnek median ve yaklaşık 20 ms
  süreklilik doğrulamasıyla trigger sınırlarına ulaşmadan engeller.
- Wi-Fi ve BLE slave'lere ortak, ileri tarihli tetikleme zamanı gönderilir.
- Telefonda BLE üzerinden BPM ve basınç aralığı ayarlanabilir.
- Filtreli batarya voltajı ve lookup-table tabanlı yüzde tahmini (seri port ve
  BLE Battery Service).
- Parola korumalı Wi-Fi OTA firmware yükleme desteği.
- PC'den parola kontrollü BLE OTA firmware yükleme desteği.
- Manşet 30 dakika boyunca 5 mmHg veya altında kalınca Wi-Fi/BLE kapanır; basınç
  5 mmHg'yi geçince otomatik olarak yeniden açılır.

## Donanım ve pinler

Derleme profili ESP32-S3 **N16R8** modülünü (16 MB QIO flash, 8 MB octal PSRAM,
3.3 V) hedefler. Farklı bir modül kullanılacaksa
[`platformio.ini`](platformio.ini) içindeki kart, bellek tipi, flash boyutu ve
bölüm tablosu da değiştirilmelidir.

| İşlev | Pin | Yön | Not |
|---|---|---|---|
| Harici basınç sensörü | GPIO2 | Analog giriş | ADC1_CH1, 11 dB. 3.3 V besleme, 0.2–2.7 V = 0–40 kPa, 500 Hz |
| Pil gerilimi (BAT_ADC) | GPIO4 | Analog giriş | ADC1_CH3. Bölücü oranı **×3** (`BATTERY_DIVIDER_RATIO`) |
| Yerel nabız LED'i (isteğe bağlı) | `PULSE_LED_PIN` | Dijital çıkış | Varsayılan `-1` (kapalı) |
| BLE durum LED'i | GPIO6 (`BLE_STATUS_LED_PIN`) | Dijital çıkış | Aktif-yüksek, seri dirençle GND'ye |
| USB D− / D+ | GPIO19 / GPIO20 | USB | Yükleme ve seri monitör, 115200 baud, USB CDC |

PCB notları:

- İki analog giriş ADC1'de kalmalıdır; ADC2, Wi-Fi açıkken kullanılamaz.
- GPIO26–32 flash'a, GPIO33–37 octal PSRAM'e ayrılmıştır. GPIO0, 3, 45 ve 46
  strapping pinleridir; LED veya başka bir çıkış için kullanmayın.
- Hareket sensörü (IMU) ve I²C hattı yoktur; bekleme modu yalnızca GPIO2'deki
  basınç sensörünü kullanır.
- Pil şarj tahmini ETA6098, 82 kΩ ISET (500 mA) ve 1500 mAh Power-Xtra PX103049
  hücresinin ölçülmüş şarj koşusunu varsayar;
  farklı şarj devresi veya pilde ilgili `BATTERY_*` sabitleri güncellenmelidir.

### Yerel nabız LED'i

Ekransız sürümde sentetik nabzı yerinde görmek için bir LED bağlanabilir. Boş,
strapping olmayan bir GPIO seçin (örneğin GPIO5) ve
[`include/config.h`](include/config.h) içinde ayarlayın:

```cpp
#define PULSE_LED_PIN 5
#define PULSE_LED_ACTIVE_LEVEL HIGH
```

LED her sentetik nabızda, slave'lerin ilk fazını belirleyen aynı planlanmış
`executeAt` zamanında 150 ms yanar. Basınç DIA–SYS aralığı dışındayken, OTA
sırasında ve standby girişinde sönük kalır. Akım sınırlama direnci kullanın;
yüksek akımlı bir yük için araya transistör veya MOSFET koyun.

### BLE durum LED'i

GPIO6'daki LED, master'ın BLE slave'e (`RTCOUND20180`) bağlantısını gösterir:

| Durum | LED |
|---|---|
| BLE açık, slave bağlı değil veya henüz senkron değil | Hızlı yanıp söner (150 ms yanık / 150 ms sönük) |
| Slave bağlı ve senkron (seri satırda `ble=OK`) | Yavaş yanıp söner (1 s yanık / 1 s sönük) |
| BLE kapalı (açılış öncesi ve bekleme modu) | Sönük |

LED Arduino döngüsünden sürülür; 500 Hz sensör görevine iş eklemez. Pin ve
aktif seviye [`include/config.h`](include/config.h) içindeki
`BLE_STATUS_LED_PIN` ve `BLE_STATUS_LED_ACTIVE_LEVEL` değerleridir (`-1`
kapatır). Yanıp sönme süreleri [`src/main.cpp`](src/main.cpp) içindeki
`BLE_LED_SEARCH_HALF_PERIOD_MS` ve `BLE_LED_SYNCED_HALF_PERIOD_MS` sabitleridir.
LED'i seri dirençle (örneğin 330 Ω–1 kΩ) GPIO6 ile GND arasına bağlayın.

Aktif topoloji:

- Wi-Fi slave: mantıksal ID 1, varsayılan GPIO2 active-high.
- BLE slave: mantıksal ID 2, reklam adı `RTCOUND20180`, GPIO4 active-low.

Master BLE tarafinda ayni anda tek fiziksel slave baglantisi kullanir ve bu
baglanti trigger state almak icin tek basina yeterlidir. Reklam adinin
`RTCOUND20180` ile birebir eslesmesi gerekir.

### Bağlanacak BLE cihazının adını ayarlama

Master'ın bağlanacağı fiziksel slave'in BLE reklam adı
[`include/config.h`](include/config.h) içindeki `BLE_NODE_NAME` değeridir:

```cpp
#define BLE_NODE_NAME "RTCOUND20180"
```

Başka bir BLE slave kullanılacaksa tırnak içindeki değer, o cihazın yayınladığı
BLE adına birebir değiştirilmelidir; büyük/küçük harf ve boşluklar önemlidir.
Bu depodaki BLE slave de kullanılacaksa onun
[`slave2_ble/include/config.h`](slave2_ble/include/config.h) dosyasındaki
`NODE_NAME` aynı değer olmalıdır:

```cpp
#define NODE_NAME "RTCOUND20180"
```

İsim değişikliğinden sonra master ve adı değiştirilen slave yeniden derlenip
kendi kartlarına yüklenmelidir. Master yalnızca `BLE_NODE_NAME` ile eşleşen tek
fiziksel slave'e bağlanır; telefon kontrol uygulamasının gördüğü master reklam
adı ise bundan bağımsız olarak `BP_TRIGGER_MASTER` kalır.

## Basınca bağlı bekleme modu

Ekransız master'da hareket sensörü yoktur. Kullanım sinyali olarak manşet
basıncı kullanılır.

**Uykuya giriş:** Basınç 30 dakika boyunca 5 mmHg veya altında kalırsa yeni
tetikler engellenir, yerel nabız LED'i söner ve son planlanmış tetik
tamamlanır. Ardından ESP32-S3 deep sleep'e girer. Bu tek adımda Wi-Fi AP, BLE
ve OTA kapanır. Kart 250 ms sonra radyoları açmadan uyanır ve bekleme
döngüsüne geçer.

**Bekleme döngüsü:** Kart light sleep'tedir. 250 ms'de bir uyanır, basınç
ADC'sinden 16 örneğin ortalamasını alır ve hemen tekrar uyur. 5 mmHg sıfırın
yalnızca ~35 mV üstüne denk geldiği için tek örnek yerine ortalama kullanılır.
RAM korunur. Basınç sensörü beklemede de beslenmeye devam eder; bu modda
tüketimin büyük kısmı sensörün kendi akımıdır.

**Uyanma:** Ortalama 5 mmHg'yi geçtiği anda Wi-Fi AP, OTA ve BLE başlatılır,
500 Hz sensör görevi çalışır ve slave'ler yeniden bağlanır. Basınç en geç
~250 ms içinde algılanır. Slave'lerin bağlanıp senkronize olması birkaç saniye
sürebilir; manşet çok hızlı şişirilirse ilk nabızlar kaçabilir.

**30 dakikalık sayacı sıfırlayanlar:**

- 5 mmHg üzerindeki basınç.
- Aktif sentetik nabız (DIA 5 mmHg'nin altına ayarlanmış olsa bile).
- Telefondan gelen her BLE komutu. Manuel tetikler manşet boşken de
  kullanılabildiği için bu gereklidir.
- Wi-Fi veya BLE OTA aktarımı.

**Sıfır noktası:** Kart basınçlı manşetle uyandığı için başlangıçtaki otomatik
sıfırlama tekrar yapılmaz. Manşet boşken ölçülen sıfır ve BLE'den girilen
BPM/basınç aralığı RTC belleğinde tutulur; seri portta
`[SENSOR] zero ... mV kept from before standby` satırı görünür. Sıfırı yeniden
ölçmek için kartı manşet boşken reset'leyin.

**USB:** Kart bir bilgisayara USB ile bağlıysa (USB SOF algılanıyorsa) light
sleep yerine normal bekleme kullanılır. Böylece seri port ve USB yüklemesi
açık kalır; USB beslemesinde tasarrufun önemi yoktur. Şarj adaptöründe veya
pilde light sleep kullanılır.

**Beklemedeyken:** Telefon uygulaması bağlanamaz ve OTA yapılamaz. Kartı
uyandırmak için manşeti 5 mmHg'nin üzerine sıkın ya da reset'e basın. Reset
normal açılış yapar, bu yüzden manşet o sırada boş olmalıdır.

Eşik ve süreler [`src/main.cpp`](src/main.cpp) içindeki
`STANDBY_PRESSURE_MMHG`, `STANDBY_TIMEOUT_MS` ve `STANDBY_CHECK_INTERVAL_MS`
sabitleridir.

## BLE telefon kontrolü

BLE reklam adı: `BP_TRIGGER_MASTER`

Telefon uygulaması UTF-8 metni `CONTROL_RX_UUID` karakteristiğine yazar:

- `72,130,60`: BPM=72, üst sınır=130 mmHg, alt sınır=60 mmHg.
- `100`: yalnızca BPM'i 100 yapar; mevcut sınırlar korunur.
- `TRIGGER`, `SMALL`, `WIFI`, `BLE`, `SMALL WIFI`, `SMALL BLE`: manuel tetikler.

Sınırlar: BPM 30–240, sistolik 31–300, diyastolik 1–sistolik-1.
UUID'ler [include/protocol.h](include/protocol.h) içindedir. Çalışma sırasında
gelen BLE ayarları bilerek yalnızca RAM'de tutulur; flash yazısının 500 Hz
basınç örneklemesini duraklatması önlenir.

Master batarya yüzdesi ayrıca standart BLE Battery Service `0x180F` içindeki
Battery Level `0x2A19` karakteristiğinden tek bayt `0..100` olarak okunabilir.
Yüzde değiştiğinde aynı karakteristik notification da gönderir; ilk geçerli
BAT_ADC ölçümü tamamlanmadan önce değer `0` olur.

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
python -m platformio run -e esp32-s3-headless-ota -t upload
Remove-Item Env:PLATFORMIO_UPLOAD_FLAGS
```

OTA başladığında yeni trigger üretimi ve otomatik standby geçici olarak durur.
Başarılı aktarım sonunda S3 otomatik yeniden başlar. OTA sırasında bağlantının
kesilmemesi için bilgisayarın `ECG_MASTER` ağından ayrılmaması gerekir.

### PC üzerinden BLE OTA yükleme

BLE OTA destekli firmware ilk kez USB ile yüklenmelidir. Sonraki güncellemelerde
bilgisayarın Bluetooth'u üzerinden, Wi-Fi ağına bağlanmadan yükleme yapılabilir.
Cihaz bekleme modundaysa önce manşeti 5 mmHg'nin üzerine sıkarak uyandırın.

```powershell
python -m platformio run -e esp32-s3-headless
python tools/ble_ota_upload.py
```

Araç varsayılan olarak
`.pio/build/esp32-s3-headless/firmware.bin` dosyasını ve
`BP_TRIGGER_MASTER` cihazını kullanır. Bir başka dosya veya parola için:

```powershell
$env:BP_BLE_OTA_PASSWORD="YEREL_OTA_PAROLASI"
python tools/ble_ota_upload.py path\to\firmware.bin
Remove-Item Env:BP_BLE_OTA_PASSWORD
```

Yalnızca `firmware.bin` kullanın; birleşik `firmware.factory.bin` BLE OTA için
uygun değildir. Aktarım her parçayı cihazdan ACK aldıktan sonra ilerletir; toplam
boyut, paket sırası, CRC32 ve ESP firmware imzası doğrulanmadan yeni bölüm boot
edilmez. Aktarım sırasında trigger üretimi, yerel nabız LED'i, BLE slave taraması
ve otomatik standby geçici olarak durur.

BLE OTA parolası `BLE_OTA_PASSWORD` ile ayrı tanımlanabilir; tanımlanmazsa
`OTA_PASSWORD` kullanılır. Bu parola yakındaki yanlışlıkla yapılan yüklemeleri
engeller ancak mevcut bağlantı eşleştirme/şifreleme zorunlu tutmadığı için güçlü
kriptografik kimlik doğrulama değildir. Güncellemeyi güvenilen fiziksel ortamda
yapın ve özel kurulumlarda ignored `include/secrets.h` ile benzersiz parola seçin.

## Seri durum satırı

Ekran olmadığı için durum, USB CDC seri portunda (115200 baud) her 500 ms'de bir
yazdırılır. Başlangıçtaki otomatik sıfırlama bitene kadar bu satır gelmez;
manşet sıfırlama sırasında boş olmalıdır. Satırın biçimi (değerler örnektir):

```text
[SENSOR] adc=412 delta=3 mV=205 zero_mV=203 mmHg=0.15 sat=0 sps=500 bpm=72 range=60-130 pulse=0 wifi=OK ble=SYNC bat=3912mV/66% charge_est=0
```

| Alan | Anlamı |
|---|---|
| `adc`, `delta`, `mV`, `zero_mV` | Ham ADC, sıfıra göre fark, kalibre mV ve başlangıçta öğrenilen sıfır |
| `mmHg`, `sat` | Temizlenmiş basınç ve ADC doygunluğu |
| `sps` | Ölçülen örnekleme hızı; 500 civarında olmalıdır |
| `bpm`, `range` | Sentetik BPM ve DIA–SYS aralığı |
| `pulse` | `1`: basınç aralıkta, sentetik nabız aktif |
| `wifi`, `ble` | `OK` senkron, `SYNC` bağlı fakat senkron değil, `OFF` bağlı değil |
| `bat`, `charge_est` | Pil gerilimi/yüzdesi ve şarj tahmini aktifliği |

## Batarya ölçümü

GPIO4, BAT_ADC direnç bölücüsünü okur. Her ölçüm grubu için 32
örnek 20 ms aralıklarla alınır; böylece basınç sensörü tek seferde bloke edilmez.
×3 bölücü katsayısı ve düşük geçiren filtre uygulanır. Yüzde doğrusal `map()`
ile değil, 3.20–4.20 V Li-Po eğrisi üzerinde
lookup table + interpolasyonla hesaplanır. USB/şarj sırasında okunan gerilim
şarj devresinden etkilenir; bu sapma ölçülmüş bir şarj/dinlenmiş gerilim
haritasıyla telafi edilir.

### Feature: Battery level while charging fix

`v2.1.0` sürümünde BAT_ADC voltajındaki ani
yukarı sıçrama doğrudan yüzde artışı kabul edilmez. Referans şemadaki ETA6098
`ISET` direnci 82 kΩ'dur ve şarj akımını 500 mA'e ayarlar. Yazılım, kablo
takılmadan önceki lookup-table SOC değerini korur; yükseliş üç ardışık ölçümle
doğrulanınca SOC'yi ölçülmüş bir CC hızı ve CC/CV son-dolum yavaşlamasıyla zaman
üzerinden ilerletir (bkz. [Şarj sırasında yüzde ilerleyişi](#şarj-sırasında-yüzde-ilerleyişi)). Böylece şarj sırasında görülen
4.20 V, gerçek batarya hemen dolmuş gibi yorumlanmaz. USB çıkarıldığında 70 mV veya daha büyük
düşüş hızlı uygulanır ve tekrar batarya-only voltaj eğrisine dönülür. Tahmini
şarj durumu aktifken seri durum satırında `charge_est=1` görünür.

Farklı bir pil veya şarj devresinde `BATTERY_CHARGE_CC_PERCENT_PER_HOUR`,
`batteryChargeTaperFactor()` ve `BATTERY_CHARGE_REST_MAP` o hücrenin ölçülmüş
şarj koşusuna göre yeniden fit edilmelidir. Yaşlanma, sıcaklık ve gerçek kapasite
nedeniyle bu yöntem fuel-gauge entegresi kadar kesin değildir, fakat şarj
gerilimini doğrudan yüzdeye çevirmekten belirgin biçimde daha gerçekçidir.

### Şarj sırasında yüzde ilerleyişi

Gösterge kablo takılı değilken yüzdeyi 3.20–4.20 V OCV eğrisinden okur. Şarj
sırasında gösterilen yüzdenin kablo çekildikten sonra aynı yere oturması için
zaman modelinin de **bu eğrinin yüzdesi** cinsinden, hücrenin kendi hızında
ilerlemesi gerekir. Etiket değeriyle hesap (500 mA × %98 / 1500 mAh) 32.7 %/saat
verir; PX103049'un ölçülmüş şarj koşusunda dinlenmiş gerilimin OCV yüzdesi ise
sabit akım bölgesinde (oturmuş 6.–10. satırlar) **61.3 %/saat** artar. Etiket
kapasitesi, kalibre edilmemiş şarj akımı ya da genel OCV eğrisi bu oranda
sapmaktadır; ölçüm deposu hangisi olduğunu açık bırakır (sınır 8). Firmware bu
yüzden kapasiteden türetilmiş bir hız yerine ölçülmüş hızı kullanır
(`BATTERY_CHARGE_CC_PERCENT_PER_HOUR`).

CV yavaşlaması da aynı koşudan fit edilmiştir. Şarj/dinlenmiş farkı %55
civarında kapanmaya başlar (139 → 51 mV) ve artış CC hızının şu oranlarına iner:

| OCV yüzdesi | Ölçülen hız | Katsayı |
|---:|---:|---:|
| < %55 | 61.3 %/saat | 1.00 |
| %55–66 | 42.0 %/saat | 0.69 |
| %66–80 | 25.3 %/saat | 0.41 |
| ≥ %80 | 8.4 %/saat | 0.14 |

%89 üstü ölçülmemiştir (koşu 4140 mV'ta takılmıştır); son ölçülen katsayı
devam eder. Harita 4100 mV'ta (~%82) bırakıldıktan sonra %100'e ~2 saat sürer.

Firmware algoritması host tarafında ölçülen koşu üzerinde yeniden oynatıldı;
gösterilen yüzde ile aynı andaki dinlenmiş gerilimin OCV yüzdesi karşılaştırıldı:

| Kalibrasyon | En büyük sapma | Son nokta (%88) |
|---|---:|---:|
| 500 mA / 1500 mAh, eski %85/93/98 bantları | −27 puan | +2 |
| 500 mA / 800 mAh, eski bantlar | −3 puan | +12 (%100 gösterir) |
| **Ölçülmüş hız + fit edilmiş yavaşlama** | **−3 puan** | **0** |

Fit'e katılmayan bağımsız ikinci koşuda (run2 ve run2b) sapma, tek bir −5.4
puanlık nokta dışında 3.5 puanın içindedir. Sapma tutarlı biçimde negatiftir:
gösterge biraz geride kalır, kablo çekilince aşağı değil yukarı oturur. Ölçüm
düzeneğinin şarj akımı kaydedilmemiştir; ölçülen 234 mΩ iç direnç ve ~140 mV CC
sapması ~500–600 mA'e işaret eder, bu da kartın 500 mA ISET değeriyle uyumludur.

### Şarj gerilimi → dinlenmiş gerilim haritası

CC/CV zaman modelinin üstüne, ölçülmüş bir gerilim haritası tavan olarak
uygulanır. Kaynak veri
[`ardaacala/powerxtra-px103049-voltage-map`](https://github.com/ardaacala/powerxtra-px103049-voltage-map)
deposundadır: kartta kullanılan Power-Xtra PX103049 (3.7 V / 1500 mAh Li-Po)
hücresinde, şarj sürerken okunan gerilim ile şarj kesildikten 60 s sonra okunan
gerilim 13 noktada eşleştirilmiştir (deponun `measurements.csv` dosyası, ilk şarj
koşusu). Aynı depodaki PX803035 (800 mAh, `-800mah` ekli) verisi bu hücre için
kullanılmaz.

Kablo takılıyken iç direnç ve polarizasyon BAT_ADC'yi açık devre geriliminin
belirgin biçimde üstüne taşır: sabit akım boyunca ~140 mV, şarj sabit gerilime
geçip akım azaldıkça ~51 mV. Ham gerilimi doğrudan OCV eğrisine vermek,
yarı boş bir hücreyi neredeyse dolu göstermenin sebebidir. Firmware bu yüzden
önce şarj gerilimini dinlenmiş karşılığına çevirir, sonra mevcut 3.20–4.20 V
eğrisinden yüzdeye geçer (değerler firmware'in tamsayı hesabıyla):

| Şarjda okunan | Ham eğri | Haritalı | Fark |
|---:|---:|---:|---:|
| 3650 mV | %27 | %10 | 17 puan |
| 3721 mV | %38 | %17 | 21 puan |
| 3900 mV | %65 | %44 | 21 puan |
| 4020 mV | %82 | %66 | 16 puan |
| 4100 mV | %90 | %82 | 8 puan |

`BATTERY_CHARGE_REST_MAP` içindeki 13 ölçüm çifti, deponun 10 mV adımlı
`lookup.csv` tablosunu 0.05 mV içinde yeniden üretir; ölçülen 3426–4140 mV
aralığının dışına **ekstrapolasyon yapılmaz**.

Harita gösterilen değerin kendisi değil, **tavanıdır**. Görünen eğriyi hâlâ CC/CV
zaman modeli çizer; harita yalnızca modelin hücrenin kendi geriliminin
desteklediğinden ileri gitmesini engeller. Bunun iki sebebi var: kaynak ölçümde
her nokta yalnızca 60 s dinlendirilmiştir, bu yüzden 3.35–3.52 V aralığındaki
dinlenmiş değerler bir miktar yüksek kalmıştır; ayrıca veri oda sıcaklığında tek
bir hücrenin tek koşusudur. Şarj oturumu boyunca
ayrıca bir taban tutulur; gösterge kablo takılıyken hiçbir zaman geri adım atmaz.

`BATTERY_CHARGE_MAP_RELEASE_MV` (4100 mV) üzerinde tavan bırakılır: bu noktadan
sonra ETA6098 sabit gerilim aşamasındadır, gerilimi şarj IC'si sabitler ve artık
doluluğu takip etmez. Son bölümü CC/CV modeli tek başına %100'e taşır. Eşik,
kaynak koşunun takıldığı 4140 mV platosunun bir harita adımı altındadır; aynı
koşunun belgelediği ±20–30 mV mutlak ADC kaymasına pay bırakır.

### İkinci şarj tespiti: sürekli yükseliş

Birincil tespit, kablo takıldığında BAT_ADC'de oluşan **ani adımı** yakalar ve üç
ölçümde (~15 s) doğrular. Ama bir boşluk bırakır: **sistem USB zaten takılıyken
açılırsa ortada yakalanacak bir adım yoktur.** O durumda şarj modu hiç devreye
girmez, yüzde ham şarj geriliminden hesaplanır ve gereğinden yüksek görünür.

İkinci bir yol bu boşluğu donanım eklemeden kapatır. Dayandığı fizik şu: sabit
akım şarjı bu paketi her beş dakikada 30–45 mV yukarı taşır ve bunu saatlerce
sürdürür; buna karşılık yük kalktığında oluşan dinlenme toparlanması bir iki
dakikada biter ve düzleşir. Ayırt edici olan tek bir yükseliş değil, yükselişin
**sürmesi**.

Bu yüzden 5 dakikalık pencereler kullanılır. Bir pencere ancak sonunda en az
20 mV artış görülürse sayılır; **arka arkaya iki pencere** gerekir. Düşüş
kontrolü pencere başlangıcına göre değil **koşan tepe noktasına** göre yapılır —
kritik olan kısım budur, çünkü "yüksel, sonra düzleş" şekli pencere boyunca net
kazanç gösterse bile tepeden gerileme sayacı sıfırlar. Toplam gecikme yaklaşık
10–15 dakikadır; normal kablo takma durumunu hâlâ hızlı adım tespiti karşılar.

Bu yol devreye girdiğinde güvenilir bir şarj-öncesi okuma **yoktur** — zaten tam
da bu yüzden devreye girmiştir. Bu durumda çapayı şarj/dinlenmiş gerilim
haritası sağlar ve taban sıfırda bırakılır, böylece harita ham gerilimin şişirdiği
yüzdeyi aşağı çekebilir. Düzeltme ani değil, güncelleme başına 1 puan adımla
yürüyerek yapılır; kablo takılıyken geriye sıçrayan bir gösterge arıza gibi
okunur.

Doğrulama host tarafında dokuz senaryoyla yapıldı: soğuk açılış, dinlenme
toparlanması, normal kablo takma regresyonu, normal deşarj, ±8 mV ADC gürültüsü,
kablo çıkarma, yetmeden kesilen yükseliş ve yavaş toparlanma. Bu doğrulama
1500 mAh PX103049 haritasıyla, ölçülmüş şarj hızından önceki zaman modeliyle
yapılmıştır; soğuk açılış senaryosunda gösterge %38'den başlayıp haritanın işaret
ettiği %36'ya iniyordu, aynı noktada ham eğri %65 derdi. Ölçülmüş hız ve CV
yavaşlamasıyla bu dokuz senaryo yeniden koşulmadı; şarj ilerleyişi yukarıdaki
oynatmayla doğrulandı.

**Bilinen sınır:** cihaz şarjın sabit gerilim kuyruğundayken (>4100 mV) açılırsa
artış hızı eşiğin altında kalır ve bu yol tetiklenmez. Orada sapma zaten küçüktür
— ham eğri %94, gerçek ~%88. Kesin çözüm için VBUS veya `STAT` hattının bir
GPIO'ya bağlanması gerekir.

Firmware ETA6098 `STAT` çıkışını okumaz; yazılım yalnızca BAT_ADC ile şarj
kablosunu kesin olarak algılayamaz. Sistem USB zaten takılıyken soğuk açılırsa
geçmiş bir batarya-only referansı yoktur. Yukarıdaki sürekli yükseliş tespiti bu
durumu 10–15 dakika içinde telafi eder, fakat anlık ve kesin bir cevap ancak ayrı
bir VBUS/STAT bağlantısıyla mümkündür. Yeni PCB'de bu hatlardan biri boş bir
GPIO'ya bağlanırsa, kullanılabilmesi için ayrıca firmware desteği gerekir.

## Proje yapısı

- `src/main.cpp`: ADC, sahte nabız, yerel nabız LED'i, BLE durum LED'i,
  basınca bağlı bekleme, seri durum satırı ve görev dağılımı.
- `src/ui_compat.cpp`: Wi-Fi modülünün eski UI çağrıları için boş uyumluluk
  fonksiyonları.
- `src/wifi_master.cpp`: Wi-Fi AP, TCP slave ve saat senkronizasyonu.
- `src/ble_master.cpp`: BLE slave ve telefon GATT kontrolü.
- `src/ble_ota_service.cpp`: PC BLE OTA protokolü ve Core 0 flash yazma görevi.
- `tools/ble_ota_upload.py`: Windows/macOS/Linux için Bleak tabanlı PC yükleyici.
- `include/config.h`: ağ, cihaz kimliği, pin ve zamanlama sabitleri.
- `include/protocol.h`: Wi-Fi/BLE paketleri ve UUID'ler.
- `AGENTS.md` / `CLAUDE.md`: Codex ve Claude için güvenli çalışma kuralları.
- `slave1_wifi`: Wi-Fi slave firmware'i.
- `slave2_ble`: GPIO4 active-low BLE slave firmware'i.

Master ve iki slave GitHub Actions ile her push ve pull request'te derlenir. GitHub
release kaynak kodu içerir; cihaz parolasını firmware içine gömmemek için herkese
açık release'e hazır `.bin` dosyası eklenmez.
