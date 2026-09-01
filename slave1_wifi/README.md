# ECG Wi-Fi Slave

Bu proje `ECG_MASTER` SoftAP agina baglanir ve `192.168.4.1:3333`
adresindeki master ile TCP uzerinden saat senkronizasyonu ve ECG trigger
paketlerini paylasir.

## Ortamlar

- `slave-gpio4-active-low`: Eski LED_NODE_1 donanimi (varsayilan)
- `slave-gpio2-active-high`: Eski LED_NODE_2 donanimi

Ornek yukleme:

```powershell
pio run -e slave-gpio4-active-low -t upload --upload-port COM3
```

Ayni ortami istenen sayida uyumlu slave karta yukleyebilirsiniz. Her slave
master'a bagimsiz TCP istemcisi olarak baglanir.
