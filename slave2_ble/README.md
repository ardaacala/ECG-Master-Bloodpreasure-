# Slave 2 BLE

Bu ayri PlatformIO projesi master'a `RTCOUND20180` adiyla BLE uzerinden baglanir.
Wi-Fi kullanmaz. Slave 2 donanimina uygun olarak LED cikisi GPIO4 ve
aktif-dusuktur.

Derleme:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -d slave2_ble
```
