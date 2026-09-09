# Changelog

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
