#!/usr/bin/env python3
"""
Tuya MCU emulator for bench-testing the Skyfan Zigbee stick without a fan.

It plays the fan's own controller on the stick's UART: answers heartbeats, product info
and network status, accepts datapoint commands (0x06) and reports state back (0x07) the
way the real fan does. Every accepted command counts as one beep, because the real fan
beeps once per datapoint command.

Wiring (3.3 V USB-UART adapter, GND common, stick powered from its own USB):
    adapter TX -> stick UART RX (GPIO17 / D7)
    adapter RX -> stick UART TX (GPIO16 / D6)
    adapter GND -> stick GND
Check the pads against your board; the XIAO ESP32-C6 uses Serial0 on GPIO16/17.

Usage:
    pip install pyserial
    python mcu_emulator.py COM7            # 9600 baud (the stick tries 9600 first)
    python mcu_emulator.py --selftest      # no hardware: run against a fake stick

Console commands while running:
    status                 print the emulated fan state
    mark                   zero the beep counter (do this just before a test)
    beeps                  print the beep counter
    remote on|off          simulate the fan's own RF remote switching it
    remote speed N         simulate the RF remote setting speed 1-5
    remote dir             simulate the RF remote toggling direction
    remote light           simulate the RF remote toggling the light
    quit
"""
import argparse
import json
import sys
import threading
import time

PRODUCT_ID = "pktxz1vynowmavuc"  # Fan + light model, as in SkyfanConfig.h

CMD_HEARTBEAT, CMD_PRODUCT, CMD_WORKMODE, CMD_NETWORK = 0x00, 0x01, 0x02, 0x03
CMD_SEND, CMD_REPORT, CMD_QUERY = 0x06, 0x07, 0x08

T_BOOL, T_VALUE, T_ENUM = 0x01, 0x02, 0x04
DP_FAN_SWITCH, DP_FAN_MODE, DP_FAN_SPEED, DP_FAN_DIR = 1, 2, 3, 8
DP_LIGHT_SWITCH, DP_LIGHT_DIMMER, DP_LIGHT_TEMP = 15, 16, 19

DP_TYPES = {
    DP_FAN_SWITCH: T_BOOL, DP_FAN_MODE: T_ENUM, DP_FAN_SPEED: T_VALUE, DP_FAN_DIR: T_ENUM,
    DP_LIGHT_SWITCH: T_BOOL, DP_LIGHT_DIMMER: T_VALUE, DP_LIGHT_TEMP: T_ENUM,
}
DP_NAMES = {
    1: "fan_switch", 2: "fan_mode", 3: "fan_speed", 8: "fan_direction",
    15: "light_switch", 16: "light_dimmer", 19: "light_colour_temp",
}


def checksum(b):
    return sum(b) & 0xFF


def frame(cmd, data=b"", version=0x03):
    body = bytes([0x55, 0xAA, version, cmd, len(data) >> 8, len(data) & 0xFF]) + data
    return body + bytes([checksum(body)])


def dp_bytes(dpid, value):
    t = DP_TYPES[dpid]
    if t == T_VALUE:
        return bytes([dpid, t, 0, 4]) + int(value).to_bytes(4, "big")
    return bytes([dpid, t, 0, 1, int(value) & 0xFF])


class Fan:
    def __init__(self, speed_wakes=False):
        self.speed_wakes = speed_wakes
        self.state = {DP_FAN_SWITCH: 0, DP_FAN_MODE: 0, DP_FAN_SPEED: 1, DP_FAN_DIR: 0,
                      DP_LIGHT_SWITCH: 1, DP_LIGHT_DIMMER: 5, DP_LIGHT_TEMP: 0}
        self.beeps = 0

    def all_dps(self):
        return b"".join(dp_bytes(d, v) for d, v in self.state.items())


class Emulator:
    def __init__(self, port, fan, log=print):
        self.port, self.fan, self.log = port, fan, log
        self.lock = threading.Lock()
        self.buf = bytearray()
        self.first_heartbeat = True
        self.last_product = 0.0
        self.running = True

    def t(self):
        return time.strftime("%H:%M:%S")

    def send(self, cmd, data=b""):
        with self.lock:
            self.port.write(frame(cmd, data))

    def report(self, *dpids):
        self.send(CMD_REPORT, b"".join(dp_bytes(d, self.fan.state[d]) for d in dpids))

    def feed(self, chunk):
        self.buf += chunk
        while True:
            i = self.buf.find(b"\x55\xaa")
            if i < 0:
                self.buf.clear()
                return
            del self.buf[:i]
            if len(self.buf) < 7:
                return
            n = (self.buf[4] << 8) | self.buf[5]
            total = 6 + n + 1
            if len(self.buf) < total:
                return
            pkt = bytes(self.buf[:total])
            del self.buf[:total]
            if checksum(pkt[:-1]) != pkt[-1]:
                self.log(f"{self.t()} RX bad checksum {pkt.hex(' ')}")
                continue
            self.handle(pkt[3], pkt[6:-1])

    def handle(self, cmd, data):
        f = self.fan
        if cmd == CMD_HEARTBEAT:
            self.send(CMD_HEARTBEAT, bytes([0 if self.first_heartbeat else 1]))
            self.first_heartbeat = False
        elif cmd == CMD_PRODUCT:
            # The stick replies to our product info with an empty 0x01, so answer at most once a
            # couple of seconds or the two ends would ping-pong forever.
            if time.time() - self.last_product > 2:
                self.last_product = time.time()
                self.send(CMD_PRODUCT, json.dumps({"p": PRODUCT_ID, "v": "1.0.0", "m": 0}).encode())
        elif cmd == CMD_NETWORK:
            self.log(f"{self.t()} network status from stick: {data.hex()}")
            self.send(CMD_NETWORK)
        elif cmd == CMD_QUERY:
            self.log(f"{self.t()} <- state query; reporting everything")
            self.send(CMD_REPORT, f.all_dps())
        elif cmd == CMD_SEND:
            self.handle_command(data)
        else:
            self.log(f"{self.t()} <- cmd 0x{cmd:02X} data {data.hex()}")

    def handle_command(self, data):
        f = self.fan
        if len(data) < 5:
            return
        dpid, typ, n = data[0], data[1], (data[2] << 8) | data[3]
        raw = data[4:4 + n]
        if dpid not in DP_TYPES:
            self.log(f"{self.t()} <- unknown DP {dpid}")
            return
        value = int.from_bytes(raw, "big")
        f.beeps += 1
        name = DP_NAMES[dpid]
        self.log(f"{self.t()} <- SET {name}={value}   BEEP #{f.beeps}")
        s = f.state
        if dpid == DP_FAN_SWITCH:
            s[DP_FAN_SWITCH] = 1 if value else 0
            # The fan reports its remembered speed with the switch, even while stopped
            self.report(DP_FAN_SWITCH, DP_FAN_SPEED)
        elif dpid == DP_FAN_SPEED:
            if 0 <= value <= 5:
                if value > 0:
                    s[DP_FAN_SPEED] = value
                if f.speed_wakes and value > 0:
                    s[DP_FAN_SWITCH] = 1
                    self.report(DP_FAN_SWITCH, DP_FAN_SPEED)
                else:
                    self.report(DP_FAN_SPEED)
        else:
            s[dpid] = value
            self.report(dpid)

    # Simulated RF remote: the fan changes by itself and announces it
    def remote(self, args):
        s = self.fan.state
        if args[:1] == ["on"]:
            s[DP_FAN_SWITCH] = 1; self.report(DP_FAN_SWITCH, DP_FAN_SPEED)
        elif args[:1] == ["off"]:
            s[DP_FAN_SWITCH] = 0; self.report(DP_FAN_SWITCH, DP_FAN_SPEED)
        elif args[:1] == ["speed"] and len(args) == 2:
            s[DP_FAN_SPEED] = max(1, min(5, int(args[1]))); s[DP_FAN_SWITCH] = 1
            self.report(DP_FAN_SWITCH, DP_FAN_SPEED)
        elif args[:1] == ["dir"]:
            s[DP_FAN_DIR] ^= 1; self.report(DP_FAN_DIR)
        elif args[:1] == ["light"]:
            s[DP_LIGHT_SWITCH] ^= 1; self.report(DP_LIGHT_SWITCH)
        else:
            self.log("usage: remote on|off|speed N|dir|light")
            return
        self.log(f"{self.t()} -> RF remote: {args}")

    def console(self):
        for line in sys.stdin:
            w = line.split()
            if not w:
                continue
            if w[0] == "status":
                self.log({DP_NAMES[k]: v for k, v in self.fan.state.items()})
            elif w[0] == "mark":
                self.fan.beeps = 0; self.log("beep counter zeroed")
            elif w[0] == "beeps":
                self.log(f"beeps since mark: {self.fan.beeps}")
            elif w[0] == "remote":
                self.remote(w[1:])
            elif w[0] in ("quit", "exit"):
                self.running = False
                return

    def run(self):
        threading.Thread(target=self.console, daemon=True).start()
        self.log("Emulator running. Waiting for the stick (it sends a heartbeat every second at start).")
        while self.running:
            chunk = self.port.read(64)
            if chunk:
                self.feed(chunk)


# ----------------------------------------------------------------------------- self test
class PtyPort:
    """Minimal serial-like wrapper around one end of a pty for the self test."""
    def __init__(self, fd):
        import os, select
        self.fd, self.os, self.select = fd, os, select

    def read(self, n):
        r, _, _ = self.select.select([self.fd], [], [], 0.1)
        return self.os.read(self.fd, n) if r else b""

    def write(self, b):
        self.os.write(self.fd, b)


def selftest():
    import os, pty, tty
    master, slave = pty.openpty()
    tty.setraw(slave)
    tty.setraw(master)
    out = []
    emu = Emulator(PtyPort(master), Fan(), log=lambda m: out.append(str(m)))
    threading.Thread(target=lambda: [emu.feed(emu.port.read(64)) for _ in iter(lambda: emu.running, False)],
                     daemon=True).start()

    def stick_send(cmd, data=b""):
        os.write(slave, frame(cmd, data, version=0x00))

    def stick_read(wait=0.4):
        time.sleep(wait)
        import select
        data = b""
        while select.select([slave], [], [], 0.05)[0]:
            data += os.read(slave, 256)
        return data

    def packets(b):
        res = []
        while len(b) >= 7:
            n = (b[4] << 8) | b[5]
            res.append((b[3], b[6:6 + n]))
            b = b[7 + n:]
        return res

    ok = True

    def check(name, cond):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + name)
        ok &= bool(cond)

    stick_send(CMD_HEARTBEAT)
    p = packets(stick_read())
    check("heartbeat answered, restart flag 0", p == [(CMD_HEARTBEAT, b"\x00")])
    stick_send(CMD_HEARTBEAT)
    p = packets(stick_read())
    check("second heartbeat flag 1", p == [(CMD_HEARTBEAT, b"\x01")])
    stick_send(CMD_PRODUCT)
    p = packets(stick_read())
    check("product info contains product id", p and PRODUCT_ID.encode() in p[0][1])
    stick_send(CMD_PRODUCT)
    p = packets(stick_read())
    check("product info not answered twice in a row", p == [])
    stick_send(CMD_QUERY)
    p = packets(stick_read())
    # 2 bool + 3 enum datapoints at 5 bytes, 2 value datapoints at 8 bytes
    check("query returns all 7 datapoints", p and p[0][0] == CMD_REPORT and len(p[0][1]) == 5 * 5 + 2 * 8)
    stick_send(CMD_SEND, dp_bytes(DP_FAN_SWITCH, 1))
    p = packets(stick_read())
    check("fan on reports switch=1 and speed", p and p[0][1][:5] == bytes([1, 1, 0, 1, 1]) and emu.fan.beeps == 1)
    stick_send(CMD_SEND, dp_bytes(DP_FAN_SPEED, 3))
    p = packets(stick_read())
    check("speed 3 reported", p and p[0][1] == dp_bytes(DP_FAN_SPEED, 3) and emu.fan.beeps == 2)
    stick_send(CMD_SEND, dp_bytes(DP_FAN_SWITCH, 0))
    p = packets(stick_read())
    check("off keeps remembered speed 3", p and p[0][1][-4:] == (3).to_bytes(4, "big") and emu.fan.beeps == 3)
    emu.remote(["speed", "5"])
    p = packets(stick_read())
    check("remote speed 5 wakes fan", p and emu.fan.state[DP_FAN_SWITCH] == 1 and emu.fan.state[DP_FAN_SPEED] == 5)
    emu.running = False
    print("\n".join(out))
    print("SELFTEST " + ("OK" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="serial port of the USB-UART adapter, e.g. COM7 or /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=9600)
    ap.add_argument("--speed-wakes", action="store_true",
                    help="a speed command also turns the fan on (real fan behaviour is unconfirmed)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        sys.exit(selftest())
    if not a.port:
        ap.error("give the serial port, or use --selftest")
    import serial  # pip install pyserial
    port = serial.Serial(a.port, a.baud, timeout=0.1)
    Emulator(port, Fan(a.speed_wakes)).run()


if __name__ == "__main__":
    main()
