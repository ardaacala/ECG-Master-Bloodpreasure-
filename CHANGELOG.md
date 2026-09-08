# Changelog

## Unreleased — Battery level while charging fix

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
- UI ripple/LED görünürlüğünü hem anlık sensör basıncı hem filtrelenmiş ekrandaki
  baz basınç için ayrı ayrı strict `DIA < P <= SYS` kontrolüyle fail-closed yapar.

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
