# Code Review Notes

Bu notlar yüklenen son `ecgmaster_wifi_ble.zip` kaynak ağacına göre hazırlanmıştır.

## Doğrulanan mimari

- Master: ESP32-S3, Wi-Fi + BLE eşzamanlı.
- Wi-Fi Slave 1: `DEVICE_ID=1`, `ECG_LEFT`.
- BLE Slave 2: `LED_NODE_2`.
- Wi-Fi/BLE görevleri Core 0'a pinlenmiş.
- UI/LVGL ana uygulama tarafında Core 1'de tutuluyor.
- Karma sistem hazır olma kontrolü Wi-Fi ID 1 ve BLE ID 2'nin sync durumunu birlikte kontrol ediyor.
- Ortak trigger lead time değeri Wi-Fi ve BLE önerilerinin maksimumu alınarak hesaplanıyor.
- Aynı `executeAtMasterUs` iki transporta aktarılıyor.
- ACK'ler master zaman alanına çevrilip `BLE - WiFi` farkı hesaplanıyor.

## Son sürümde dikkat çeken önemli ayrıntı

Wi-Fi Slave 1 `config.h` içindeki fallback değerleri:

```text
LED_PIN=4
LED_ACTIVE_LEVEL=LOW
```

olmasına rağmen aktif/default PlatformIO environment:

```text
slave-gpio2-active-high
```

ve build flag'leri:

```text
LED_PIN=2
LED_ACTIVE_LEVEL=HIGH
```

şeklindedir. Dolayısıyla gerçek mevcut build GPIO2 active-high kullanır.

Bu environment/build flag'leri değiştirilmeden önce donanım doğrulanmalıdır.

## Protocol ABI

Packed boyutlar:

```text
SyncRequest          13
SyncResponse         21
SyncCommit           13
WiFi TriggerPacket   26
WiFi LedOnAck        17
HelloPacket          26
DeviceStatusPacket   11
BLE TriggerPacket    18
BLE LedOnAck         13
```

BLE wire formatının Wi-Fi formatından küçük olması mevcut tasarımın bilinçli compatibility davranışıdır.

## Clock sign

Kod genelinde temel işaret sözleşmesi:

```text
offset = slave - master
```

Trigger'da offset eklenir, ACK'i master alanına getirirken çıkarılır.

## UI

Son kaynakta:

```text
UI_FRAME_INTERVAL_MS  = 16
UI_STATUS_INTERVAL_MS = 500
```

Bu nedenle eski açıklamalardaki yaklaşık 100 ms UI publish bilgisi güncel değildir.

## Repo temizliği

GitHub-ready kopyada:

- `.pio` build/cache klasörleri kaldırıldı.
- `.vscode` yerel editör klasörleri kaldırıldı.
- `core_dir = C:/pio` makineye özel ayarı kaldırıldı.
- Aktif Wi-Fi Slave 1 `slave1_wifi/` altında toplandı.
- Aktif olmayan eski `ecgslave2_wifi` repo paketine alınmadı.

Kaynak davranışı değiştirilmedi; yapılan değişiklikler repo paketleme/dokümantasyon ve taşınabilir PlatformIO yapılandırmasıyla sınırlıdır.
