# Version 2 Review Checklist

Her değişiklikten sonra aşağıdakileri kontrol edin:

- Master, Wi-Fi slave ve BLE slave hatasız derleniyor mu?
- UI görevi Core 1'de ve 30 Hz zaman çizgisinde kalıyor mu?
- Ağ/ADC işleri Core 0'da bloklamadan çalışıyor mu?
- UI ve fiziksel slave'ler aynı planlanmış timestamp'i kullanıyor mu?
- Global 200 ms duplicate guard korunuyor mu?
- Gerçek cuff nabız algılayıcısı yanlışlıkla tekrar etkinleştirildi mi?
- Sentetik nabız yalnızca ayarlanan alt/üst basınç aralığında mı?
- Sınır debounce'u aktifken bile aralık dışında ripple ve UI LED tamamen gizli mi?
- Ham basınç ile filtrelenmiş UI baz basıncı sınırın farklı taraflarındayken
  fail-closed görünürlük kapısı ripple'ı engelliyor mu?
- Ripple tepesi her beat'te sabit 1.5 mmHg ve süresi BPM periyoduyla sınırlı mı?
- BAT_ADC örnekleri burst yerine zamana yayılmış mı?
- Dokunmatik I²C arızasında sıkı retry/log döngüsü yerine backoff çalışıyor mu?
- BLE adı master ve `slave2_ble` içinde aynı mı (`LED_NODE_1`)?
- Wi-Fi ve BLE packed paket boyutları karşı uçlarla aynı mı?
- GPIO2 basınç sensörü ve GPIO4 BAT_ADC çakışmadan korunuyor mu?
- Dokunmatik/QMI8658 ortak GPIO47/48 hattı değiştirilmiş mi?
- UI task içine flash, ağ, uzun seri çıktı veya busy-loop eklendi mi?
- Gerçek parola içeren hiçbir `secrets.h` Git'e eklenmiş mi?
- Şarj SOC'si pre-charge değerden 500 mA/1500 mAh modele göre mi ilerliyor;
  USB çıkışı yeniden OCV lookup değerine hızlı dönüyor mu?

Donanım yüklemesinde ESP32-S3 master portunu VID:PID `303A:1001` ile doğrulayın;
CP210x slave portlarına master firmware'i yüklemeyin.
