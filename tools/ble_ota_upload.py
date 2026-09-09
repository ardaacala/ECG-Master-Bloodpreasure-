#!/usr/bin/env python3
"""Upload the master's PlatformIO firmware.bin over its BLE OTA service."""

from __future__ import annotations

import argparse
import asyncio
import os
import struct
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path

from bleak import BleakClient, BleakScanner


DEVICE_NAME = "BP_TRIGGER_MASTER"
SERVICE_UUID = "7d2e0011-8d7b-4f91-a8d3-4c7f2b6a1000"
CONTROL_UUID = "7d2e0012-8d7b-4f91-a8d3-4c7f2b6a1000"
DATA_UUID = "7d2e0013-8d7b-4f91-a8d3-4c7f2b6a1000"
STATUS_UUID = "7d2e0014-8d7b-4f91-a8d3-4c7f2b6a1000"

CMD_START = 1
CMD_FINISH = 2
CMD_ABORT = 3

STATUS_READY = 1
STATUS_ACK = 2
STATUS_COMPLETE = 3
STATUS_ABORTED = 4
STATUS_ERROR = 0x80

STATUS_STRUCT = struct.Struct("<BIi")
START_STRUCT = struct.Struct("<BIIB")
OFFSET_STRUCT = struct.Struct("<I")

ERRORS = {
    1: "gecersiz komut",
    2: "parola hatali",
    3: "OTA servisi mesgul",
    4: "cihaz kuyrugu dolu",
    5: "OTA partition bulunamadi",
    6: "firmware boyutu gecersiz",
    7: "paket offset/sirasi gecersiz",
    8: "flash yazma hatasi",
    9: "firmware eksik",
    10: "CRC32 uyusmazligi",
    11: "ESP firmware dogrulamasi basarisiz",
    12: "boot partition secilemedi",
}


@dataclass(frozen=True)
class Status:
    code: int
    next_offset: int
    detail: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="ESP32-S3 master firmware'ini BLE uzerinden yukler."
    )
    parser.add_argument(
        "firmware",
        nargs="?",
        default=".pio/build/esp32-s3-touch-amoled-1-43/firmware.bin",
        help="PlatformIO firmware.bin yolu",
    )
    parser.add_argument("--name", default=DEVICE_NAME, help="BLE cihaz adi")
    parser.add_argument("--address", help="Tarama yerine dogrudan BLE adresi")
    parser.add_argument(
        "--password",
        default=os.environ.get("BP_BLE_OTA_PASSWORD", "ecgmaster123"),
        help="BLE OTA parolasi (veya BP_BLE_OTA_PASSWORD ortam degiskeni)",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=180,
        help="Istenen veri parcasi boyutu; MTU'ya gore otomatik dusurulur",
    )
    parser.add_argument("--scan-timeout", type=float, default=15.0)
    return parser.parse_args()


async def wait_for_status(
    queue: asyncio.Queue[Status], expected: set[int], timeout: float
) -> Status:
    deadline = asyncio.get_running_loop().time() + timeout
    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            raise TimeoutError("Cihazdan BLE OTA cevabi zamaninda gelmedi")
        status = await asyncio.wait_for(queue.get(), remaining)
        if status.code == STATUS_ERROR:
            reason = ERRORS.get(status.detail, f"cihaz hata kodu {status.detail}")
            raise RuntimeError(
                f"BLE OTA durduruldu: {reason} (offset={status.next_offset})"
            )
        if status.code in expected:
            return status


async def find_device(name: str, address: str | None, timeout: float):
    if address:
        return address
    print(f"[BLE OTA] {name} araniyor...")
    device = await BleakScanner.find_device_by_filter(
        lambda candidate, advertisement: (
            advertisement.local_name == name or candidate.name == name
        ),
        timeout=timeout,
    )
    if device is None:
        raise RuntimeError(
            f"{name} bulunamadi; S3'u uyandirin ve Bluetooth'u kontrol edin"
        )
    return device


async def upload(args: argparse.Namespace) -> None:
    firmware_path = Path(args.firmware).resolve()
    if "factory" in firmware_path.name.lower():
        raise ValueError(
            "firmware.factory.bin BLE OTA icin kullanilamaz; firmware.bin secin"
        )
    image = firmware_path.read_bytes()
    if len(image) < 64 * 1024 or image[0] != 0xE9:
        raise ValueError(
            "Gecerli ESP32 firmware.bin bekleniyor; firmware.factory.bin kullanmayin"
        )
    if not 16 <= args.chunk_size <= 240:
        raise ValueError("--chunk-size 16 ile 240 arasinda olmali")
    password = args.password.encode("utf-8")
    if len(password) > 54:
        raise ValueError("BLE OTA parolasi en fazla 54 UTF-8 byte olabilir")
    image_crc = zlib.crc32(image) & 0xFFFFFFFF
    target = await find_device(args.name, args.address, args.scan_timeout)
    status_queue: asyncio.Queue[Status] = asyncio.Queue()
    transfer_started = False

    def on_status(_, payload: bytearray) -> None:
        if len(payload) != STATUS_STRUCT.size:
            return
        code, next_offset, detail = STATUS_STRUCT.unpack(payload)
        status_queue.put_nowait(Status(code, next_offset, detail))

    async with BleakClient(target, timeout=20.0) as client:
        control = client.services.get_characteristic(CONTROL_UUID)
        data_characteristic = client.services.get_characteristic(DATA_UUID)
        status_characteristic = client.services.get_characteristic(STATUS_UUID)
        service = client.services.get_service(SERVICE_UUID)
        if not service or not control or not data_characteristic or not status_characteristic:
            raise RuntimeError(
                "BLE OTA servisi bulunamadi; once BLE-OTA destekli firmware'i USB ile yukleyin"
            )
        await client.start_notify(status_characteristic, on_status)
        try:
            start_packet = START_STRUCT.pack(
                CMD_START, len(image), image_crc, len(password)
            ) + password
            await client.write_gatt_char(control, start_packet, response=True)
            transfer_started = True
            ready = await wait_for_status(status_queue, {STATUS_READY}, 45.0)

            mtu = max(23, int(getattr(client, "mtu_size", 23)))
            device_limit = ready.detail if ready.detail > 0 else 240
            chunk_size = min(args.chunk_size, device_limit, mtu - 7)
            if chunk_size < 16:
                raise RuntimeError(f"BLE MTU veri aktarimi icin cok kucuk: {mtu}")
            print(
                f"[BLE OTA] baglandi; {len(image)} byte, CRC32={image_crc:08X}, "
                f"MTU={mtu}, chunk={chunk_size}"
            )

            started_at = time.monotonic()
            next_report = 0
            offset = 0
            while offset < len(image):
                chunk = image[offset : offset + chunk_size]
                packet = OFFSET_STRUCT.pack(offset) + chunk
                await client.write_gatt_char(
                    data_characteristic, packet, response=True
                )
                expected_offset = offset + len(chunk)
                acknowledgement = await wait_for_status(
                    status_queue, {STATUS_ACK}, 15.0
                )
                if acknowledgement.next_offset != expected_offset:
                    raise RuntimeError(
                        "Cihaz ACK offset'i uyusmuyor: "
                        f"{acknowledgement.next_offset} != {expected_offset}"
                    )
                offset = expected_offset
                percent = offset * 100 // len(image)
                if percent >= next_report or offset == len(image):
                    elapsed = max(0.001, time.monotonic() - started_at)
                    rate_kib = offset / elapsed / 1024.0
                    print(
                        f"\r[BLE OTA] %{percent:3d}  {rate_kib:6.1f} KiB/s",
                        end="",
                        flush=True,
                    )
                    next_report = percent + 1

            await client.write_gatt_char(
                control, bytes([CMD_FINISH]), response=True
            )
            complete = await wait_for_status(
                status_queue, {STATUS_COMPLETE}, 30.0
            )
            if complete.next_offset != len(image):
                raise RuntimeError("Tamamlanma boyutu firmware ile uyusmuyor")
            transfer_started = False
            print("\n[BLE OTA] dogrulandi; S3 yeniden baslatiliyor")
            await asyncio.sleep(1.0)
        except BaseException:
            if transfer_started and client.is_connected:
                try:
                    await client.write_gatt_char(
                        control, bytes([CMD_ABORT]), response=True
                    )
                except Exception:
                    pass
            raise


def main() -> int:
    args = parse_args()
    try:
        asyncio.run(upload(args))
    except KeyboardInterrupt:
        print("\n[BLE OTA] kullanici tarafindan durduruldu", file=sys.stderr)
        return 130
    except Exception as error:
        print(f"\n[BLE OTA] HATA: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
