# Version 3 Review Checklist

Her değişiklikten sonra aşağıdakileri kontrol edin:

- Master, Wi-Fi slave ve BLE slave hatasız derleniyor mu?
- Ekran/dokunmatik kodu veya GFX/LVGL kütüphanesi geri eklenmiş mi?
- 500 Hz basınç örneklemesi (`sps`) ve ağ işleri Core 0'da bloklamadan çalışıyor mu?
- Yerel nabız LED'i ve fiziksel slave'lerin ilk fazı aynı planlanmış timestamp'i kullanıyor mu?
- State degisiklik/reconnect aninda hemen gonderiliyor mu?
- Degismeyen state `1, 2, 4, 8, 10, 10...` saniye araliklarla tekrarlaniyor ve
  backoff state degisiminde/reconnect'te sifirlaniyor mu?
- Duplicate state slave flash'ini yeniden yazmadan `UNCHANGED` ACK veriyor mu?
- Global 200 ms duplicate guard korunuyor mu?
- Gerçek cuff nabız algılayıcısı yanlışlıkla tekrar etkinleştirildi mi?
- Sentetik nabız yalnızca ayarlanan alt/üst basınç aralığında mı?
- Sınır debounce'u aktifken bile aralık dışında yerel nabız LED'i tamamen sönük mü?
- Standby girişinde ve OTA sırasında yerel nabız LED'i sönük kalıyor mu?
- BAT_ADC örnekleri burst yerine zamana yayılmış mı?
- BLE adı master ve `slave2_ble` içinde aynı mı (`RTCOUND20180`)?
- Wi-Fi ve BLE packed paket boyutları karşı uçlarla aynı mı?
- GPIO2 basınç sensörü ve GPIO4 BAT_ADC çakışmadan korunuyor mu?
- QMI8658 GPIO47/48 I²C hattı veya GPIO8 uyandırma hattı değiştirilmiş mi?
- `PULSE_LED_PIN` strapping pinlerine (0, 3, 45, 46) atanmış mı?
- Sensör görevine flash, ağ, uzun seri çıktı veya busy-loop eklendi mi?
- Gerçek parola içeren hiçbir `secrets.h` Git'e eklenmiş mi?
- Şarj SOC'si pre-charge değerden 500 mA/1500 mAh modele göre mi ilerliyor;
  USB çıkışı yeniden OCV lookup değerine hızlı dönüyor mu?

Donanım yüklemesinde ESP32-S3 master portunu VID:PID `303A:1001` ile doğrulayın;
CP210x slave portlarına master firmware'i yüklemeyin.
