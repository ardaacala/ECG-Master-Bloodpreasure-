# ECG Wi-Fi Slave

Bu proje `ECG_MASTER` SoftAP agina baglanir ve `192.168.4.1:3333`
adresindeki master ile TCP uzerinden saat senkronizasyonu ve ECG trigger
paketlerini paylasir.

## Ortamlar

- `slave-gpio2-active-high`: Aktif varsayilan, LED GPIO2 active-high.
- `slave-gpio4-active-low`: Alternatif eski donanim, GPIO4 active-low.

Ornek yukleme:

```powershell
pio run -e slave-gpio2-active-high -t upload --upload-port COM3
```

Yuklemeden once `include/secrets.example.h` dosyasini `include/secrets.h`
olarak kopyalayip master ile ayni Wi-Fi parolasini girin. Gercek secrets dosyasi
Git'e eklenmez.
