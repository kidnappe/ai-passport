#!/usr/bin/env python3
"""Install a .pap package over Passport Link BLE. Requires: pip install bleak"""
from __future__ import annotations
import argparse, asyncio, struct, zlib
from pathlib import Path

SERVICE_UUID = "0100004b-4e49-4c54-524f-505353415031"
CODE_UUID = "01000045-444f-4354-524f-505353415031"
PKG_CTRL_UUID = "01000043-474b-5054-524f-505353415031"
PKG_DATA_UUID = "01000044-474b-5054-524f-505353415031"
PKG_STATUS_UUID = "01000053-474b-5054-524f-505353415031"


def parse_code(code: str) -> int:
    compact = code.replace("-", "").replace(":", "").lower()
    if len(compact) != 12:
        raise ValueError("设备码应为 12 位十六进制 MAC 地址")
    try:
        value = int(compact, 16)
    except ValueError:
        raise ValueError("设备码包含无效字符")
    if value > 0xFFFFFFFFFFFF:
        raise ValueError("设备码超出范围")
    return value


async def install(code: str, package: Path) -> None:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError as exc:
        raise SystemExit("缺少 bleak，请先执行: pip install bleak") from exc

    device_id = parse_code(code)
    wanted_name = f"Passport-{code.upper()}"
    print(f"正在查找 {wanted_name} ...")
    device = await BleakScanner.find_device_by_filter(lambda d, ad: ad.local_name == wanted_name, timeout=12.0)
    if not device:
        raise SystemExit("未找到目标设备，请确认设备码及蓝牙距离")

    data = package.read_bytes()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    status_messages: list[str] = []

    def status_cb(_, value: bytearray) -> None:
        text = bytes(value).decode("utf-8", errors="replace")
        status_messages.append(text)
        print(f"设备: {text}")

    async with BleakClient(device) as client:
        actual_code = (await client.read_gatt_char(CODE_UUID)).decode("utf-8")
        if actual_code.upper() != code.upper():
            raise SystemExit(f"目标复核失败：设备报告 {actual_code}")
        await client.start_notify(PKG_STATUS_UUID, status_cb)
        begin = struct.pack("<BIIQ", 1, len(data), crc, device_id)
        await client.write_gatt_char(PKG_CTRL_UUID, begin, response=True)
        for offset in range(0, len(data), 180):
            await client.write_gatt_char(PKG_DATA_UUID, data[offset:offset+180], response=False)
        await client.write_gatt_char(PKG_CTRL_UUID, b"\x02", response=True)
        await asyncio.sleep(2.0)
        await client.stop_notify(PKG_STATUS_UUID)
    if not status_messages:
        print("传输结束；设备未返回通知，请在插件管理中确认安装结果")


def main() -> None:
    p = argparse.ArgumentParser(description="通过无系统配对 BLE 安装 Passport .pap")
    p.add_argument("device_code", help="例如 XXXXX-XXXXX-X")
    p.add_argument("package", type=Path)
    args = p.parse_args()
    asyncio.run(install(args.device_code, args.package.resolve()))

if __name__ == "__main__": main()
