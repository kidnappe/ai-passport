#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""抓取 ESP32-C3 串口日志到文件（带时间戳）。
用法: python capture_serial_log.py <COM端口> <输出文件> [波特率]
默认波特率 115200。Ctrl+C 停止并保存。
"""
import sys, time, datetime, serial

def main():
    if len(sys.argv) < 3:
        print("用法: capture_serial_log.py <PORT> <OUTFILE> [BAUD]")
        return 1
    port = sys.argv[1]
    outfile = sys.argv[2]
    baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

    ser = serial.Serial(port, baud, timeout=0.2)
    print(f"[capture] 打开 {port} @{baud}, 输出到 {outfile}，Ctrl+C 停止", flush=True)
    with open(outfile, "a", encoding="utf-8", errors="replace") as f:
        buf = b""
        try:
            while True:
                data = ser.read(4096)
                if not data:
                    time.sleep(0.01)
                    continue
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.rstrip(b"\r")
                    if line:
                        ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                        text = f"[{ts}] {line.decode('utf-8', errors='replace')}"
                        print(text, flush=True)
                        f.write(text + "\n")
                        f.flush()
        except KeyboardInterrupt:
            pass
        finally:
            # 写残留
            if buf:
                ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                text = f"[{ts}] {buf.decode('utf-8', errors='replace')}"
                print(text, flush=True)
                f.write(text + "\n")
            print("\n[capture] 已保存到", outfile, flush=True)
    ser.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
