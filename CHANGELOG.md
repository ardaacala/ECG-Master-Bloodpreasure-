# Changelog

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
