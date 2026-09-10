# Changelog

## Unreleased

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
