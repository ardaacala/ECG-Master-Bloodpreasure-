# Changelog

## 5.0.0 - 2026-10-05 (ekransiz / headless)

- Master'i ekransiz ozel ESP32-S3 PCB'ye tasir. CO5300 AMOLED surucusu, 30 Hz
  UI gorevi, framebuffer, tema ve dokunmatik (FT3168/FT6146) kodu kaldirilir;
  `GFX Library for Arduino` ve `lvgl` bagimliliklari projeden cikar.
- Ekrandaki nabiz LED'inin yerine istege bagli bir GPIO LED'i gelir
  (`PULSE_LED_PIN`, varsayilan `-1` kapali). LED, slave'lerin ilk fazini
  belirleyen ayni planlanmis `executeAt` zamaninda 150 ms yanar; 500 Hz sensor
  gorevinde surulur, aralik disinda, OTA sirasinda ve standby girisinde sonuk kalir.
- Ekrandaki `W:`/`B:` baglanti seridi seri durum satirina tasinir
  (`wifi=OK|SYNC|OFF ble=OK|SYNC|OFF`); `ui_fps`, `ui_age`, `ui_max` ve `fake`
  alanlari kaldirilir, sentetik nabzin aktif oldugunu gosteren `pulse=` eklenir.
- QMI8658 hareketle uyandirma korunur; I2C hatti artik yalnizca IMU icindir
  (GPIO47/48, INT1 GPIO8). IMU bulunamazsa yalnizca otomatik standby kapanir.
  Standby'da saklanan RTC ayarlarindan tema alani cikar ve magic degeri degisir.
- PlatformIO ortamlari `esp32-s3-headless` ve `esp32-s3-headless-ota` olarak
  yeniden adlandirilir; BLE OTA aracinin varsayilan `firmware.bin` yolu buna
  gore guncellenir.
- Basinc sensoru, batarya tahmini, Version 3 state trigger paket sozlesmesi,
  BLE/Wi-Fi davranisi ve slave yazilimlari degismez.

## 4.1.0 - 2026-09-15

- Sarj sirasinda gosterilen batarya yuzdesine olculmus bir sarj/dinlenmis
  gerilim haritasi tavan olarak uygulanir. 13 olcum cifti Power-Xtra PX103049
  hucresinden alinmistir (`ardaacala/powerxtra-px103049-voltage-map`) ve
  deponun 10 mV adimli lookup tablosunu 0.05 mV icinde yeniden uretir; olculen
  3426-4140 mV araliginin disina ekstrapolasyon yapilmaz.
- Sarj gerilimi once dinlenmis karsiligina cevrilir, sonra mevcut 3.20-4.20 V
  egrisinden yuzdeye gecer; sabit akim bolgesinde ham egriye gore 21 puana
  varan fazla gosterim ortadan kalkar.
- CC/CV zaman modeli korunur ve gosterilen egriyi cizmeye devam eder; harita
  yalnizca tavan koyar. 4100 mV uzerinde sabit gerilim asamasi basladigi icin
  tavan birakilir. Sarj oturumu boyunca tutulan taban sayesinde gosterge kablo
  takiliyken geri adim atmaz.
- Sarj moduna ikinci bir giris yolu ekler: 5 dakikalik pencerelerde arka arkaya
  iki kez en az 20 mV surekli yukselis. USB zaten takiliyken acilan kartta
  yakalanacak bir gerilim adimi olmadigi icin birincil tespit calismiyordu.
  Dusus kontrolu pencere basina degil kosan tepe noktasina gore yapilir; boylece
  yuk kalkinca olusan "yuksel sonra duzles" toparlanmasi yanlis pozitif vermez.
- Bu yolla acilan oturumda guvenilir sarj-oncesi okuma olmadigi icin capayi
  sarj/dinlenmis gerilim haritasi saglar ve taban sifirda birakilir. Asagi
  duzeltme guncelleme basina 1 puan adimla yurur, ani sicrama yapmaz.

## 4.0.0 - 2026-09-10

- CO5300 uzerinde ibre, LED, baglanti ve batarya katmanlarinin cakismasinda
  gorulen dikdortgen/hayalet izlerini onlemek icin dirty-region aktarimini
  kaldirir; 30 Hz UI her kareyi tek tamamlanmis framebuffer olarak gonderir.
- Tam ekran QSPI aktarim blogunu 1024 pikselden 8192 piksele cikarir ve RGB565
  byte sirasini framebuffer uzerinde tek geciste hazirlayarak ikinci surucu
  kopyasini kaldirir; olculen tam-kare UI hizi 13 FPS'den 14-15 FPS'ye cikar.
- Fiziksel BLE slave reklam filtresini gercek cihaz adi `RTCOUND20180` ile
  eslestirir; master ve `slave2_ble` varsayilanlari birlikte guncellenir.
- Tek bir senkron alicinin yeterli oldugunu netlestirir ve eski Wi-Fi hazirlik
  esigini iki cihazdan bir cihaza indirir; Wi-Fi/BLE state teslimati birbirini
  beklemez.

## 3.0.0 - 2026-09-09

- State degismese de baglanti sagligini ve kacirilan paketlerden otomatik
  toparlanmayi korumak icin yayin araligini `1, 2, 4, 8, 10, 10...` saniye
  seklinde exponansiyel yapar; state degisiminde ve reconnect'te hemen sifirlar.
- Per-beat Wi-Fi/BLE trigger paketlerini, alici ID + BPM + basinc bolgesi +
  opsiyonel gecikme iceren retained state protokoluyle degistirir.
- State'i degisiklik/reconnect aninda hemen, ardindan 1-10 saniye exponansiyel
  heartbeat ile yollar; her paketin revision ACK'ini izler.
- Wi-Fi ve BLE slave'lerde state'i semantik degisiklikte NVS'ye kaydeder,
  duplicate pakette flash yazimini atlar ve reboot sonrasinda yerel BPM uretimini
  saklanan state ile surdurur.
- `BELOW`, `INSIDE`, `ABOVE` basinc bolgelerini protokol sozlesmesine ekler;
  mevcut basit LED slave'ler yalnizca `INSIDE` bolgesinde cikis uretir.
- Periyodik saat senkronizasyonunu state tasimasindan ayri tutar ve ilk yerel
  cikisi master'in ortak effective timestamp'ine sabitler.

- PC üzerinden `BP_TRIGGER_MASTER` cihazına Wi-Fi gerektirmeden firmware gönderen
  ayrı bir BLE OTA GATT servisi ve Bleak tabanlı yükleme aracı ekler.
- Parça sırası/offset ACK'i, tam görüntü CRC32 kontrolü ve ESP OTA imaj doğrulaması
  başarısızsa çalışan partition'ı korur.
- BLE OTA sırasında sentetik tetikleme, slave taraması ve standby geçişini durdurur;
  flash yazmayı Core 0'daki bloklanan bir görevde yaparak 30 Hz Core 1 UI yapısını
  korur.

## 2.1.1 - 2026-09-09

- Basınç sensörüne Core 0 üzerinde çalışan, sabit bellekli 5 örnek median filtre
  ve 2.5 mmHg üzerindeki ani adımlar için 8 örneklik süreklilik doğrulaması ekler.
- Eski seviyeye geri dönen kısa ADC sıçramalarını UI ve sentetik BPM basınç
  sınırlarına ulaşmadan atar; kalıcı gerçek basınç değişimini yaklaşık 20 ms'de
  kabul eder. Sentetik 1.5 mmHg ripple filtre sonrasında eklenmeye devam eder.
- Master ve Wi-Fi slave için belgelenmiş temiz-kurulum varsayılanını
  `ECG_MASTER` / `ecgmaster123` yapar; ignored `secrets.h` ile yerel override
  desteğini korur.

## 2.1.0 - 2026-09-09

- BAT_ADC kaynaklı ani şarj voltajı sıçramasının yüzdeyi hemen 100'e taşımasını engeller.
- Şemadaki 82 kΩ ISET / 500 mA şarj akımıyla 1500 mAh batarya için zaman tabanlı
  ve CC/CV taper içeren SOC tahmini uygular.
- Kablo takılmadan önceki batarya-only yüzdesini şarj tahmininin başlangıcı yapar.
- USB çıkarıldığında oluşan belirgin voltaj düşüşünü hızlı uygulayarak gerçek
  batarya değerine dönüşteki eski uzun filtre gecikmesini kaldırır.
- Seri telemetriye `charge_est` tanı alanını ekler.
- Alt/üst basınç sınırının dışında kadran ripple'ını ve UI nabız LED'ini anında
  gizler; 300 ms sınır debounce'u yalnız BPM zaman çizelgesini korur.
- Sentetik ripple tepesini her beat için değişmeyen `1.5 mmHg` yapar; süresi BPM
  periyoduna göre ölçeklenmeye devam eder.
- 32 BAT_ADC okumasını tek blok yerine 20 ms aralıklarla dağıtarak periyodik
  sensör/ibre duraklamasını kaldırır ve ibre takip filtresinin gecikmesini azaltır.
- Dokunmatik I²C hatalarında sürekli yeniden deneyip log/CPU flood oluşturmak
  yerine üç hatadan sonra 2 saniyelik backoff ve kontrollü bus recovery uygular.
- Sahte ripple/LED görünürlüğünü doğrudan ölçülen basınca uygulanan strict
  `DIA < P < SYS` koşuluyla sınırlar; BLE sınır değişiminde eski UI darbesini siler.

- QMI8658 `INT1`/GPIO8 Wake-on-Motion ile 5 dakika hareketsizlikte ekran, CPU'lar,
  Wi-Fi, BLE ve tetiklemeleri kapatan deep-sleep bekleme modu ekler.
- Normal çalışmada fiziksel aktiviteyi hafif 10 Hz ivme örnekleriyle takip eder;
  dokunmayı da aktivite sayar ve WoM'u yalnızca deep sleep öncesinde kurar.
- Hareketle uyanmada çalışma ayarlarını RTC belleğinden geri yükler; IMU
  başlatılamazsa güvenli biçimde otomatik beklemeyi devre dışı bırakır.
- Master AP üzerinde parola korumalı ArduinoOTA hizmeti ekler; OTA aktarımı
  sırasında trigger üretimini ve otomatik standby'ı durdurur.

## 2.0.0 - 2026-09-08

- 30 Hz arayüzü ayrı ESP32-S3 çekirdeğine taşıdı.
- Gerçek nabız algılamasını kaldırıp kararlı sentetik BPM akışına geçti.
- UI, Wi-Fi slave ve BLE slave tetikleme zamanlarını birleştirdi.
- 200 ms tekrar tetikleme koruması ve basınç sınırı histerezisi ekledi.
- Sentetik kadran salınımını daha gerçekçi `0.6 mmHg` tepeye indirdi.
- BLE komut formatını `BPM,SYS,DIA` olacak şekilde genişletti.
- GPIO4 BAT_ADC ölçümü, Li-Po lookup table ve filtreli yüzde göstergesi ekledi.
- Dokunmatik tema değiştirme ve 2 saniyelik ekran güç hareketi ekledi.
- PlatformIO ve tüm ana bağımlılıkları tekrarlanabilir derleme için sabitledi.
