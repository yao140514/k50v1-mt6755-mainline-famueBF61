#!/usr/bin/env python3
"""Drive the k50v1 board over telnet.

Usage:
    python board.py run "cmd1" "cmd2" ...     # run shell commands, print output
    python board.py raw "text"                # send raw bytes
    python board.py put <local> <remote>      # push a file (base64 over the wire)
    python board.py get <remote>              # dump a small text file

Notes:
  * The board runs busybox ash, prompt is "~ # ".
  * We append a unique marker after every command and read until it shows up,
    so output is never truncated by timing.
  * Telnet IAC negotiation is answered with WONT/DONT (no options needed).
"""
import base64
import os
import re
import socket
import sys
import time

HOST = os.environ.get("BOARD_HOST", "192.168.137.50")
PORT = int(os.environ.get("BOARD_PORT", "23"))

IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


class Board:
    def __init__(self, host=HOST, port=PORT, timeout=20):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(1.0)
        self.buf = b""
        self.seq = 0
        time.sleep(0.2)
        self._drain(0.4)

    # ---------- telnet plumbing ----------
    def _answer_iac(self, data):
        out = bytearray()
        i = 0
        while i < len(data):
            b = data[i]
            if b != IAC:
                out.append(b)
                i += 1
                continue
            if i + 1 >= len(data):
                break
            cmd = data[i + 1]
            if cmd in (DO, DONT, WILL, WONT):
                if i + 2 >= len(data):
                    break
                opt = data[i + 2]
                if cmd == DO:
                    out += bytes([IAC, WONT, opt])
                elif cmd == WILL:
                    out += bytes([IAC, DONT, opt])
                i += 3
            elif cmd == SB:
                j = data.find(bytes([IAC, SE]), i)
                i = len(data) if j < 0 else j + 2
            else:
                i += 2
        return bytes(out)

    def _drain(self, wait=0.3):
        """Read whatever is pending.

        We deliberately do NOT answer IAC: this busybox telnetd behaves fine
        with negotiation left alone, and sending WONT/DONT confused its line
        discipline (commands arrived mangled).  The bytes are stripped in
        _clean() instead.
        """
        end = time.time() + wait
        while time.time() < end:
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.buf += chunk
            except socket.timeout:
                continue
            except OSError:
                break

    def _clean(self, data):
        # strip telnet commands, normalise CR
        data = re.sub(bytes([IAC, SB]) + b".*?" + bytes([IAC, SE]), b"", data, flags=re.S)
        data = re.sub(bytes([IAC]) + b"[\xfb-\xfe].", b"", data, flags=re.S)
        data = re.sub(bytes([IAC]) + b".", b"", data)
        text = data.decode("utf-8", "replace")
        text = text.replace("\r\n", "\n").replace("\r", "")
        return text

    # ---------- command execution ----------
    def run(self, cmd, timeout=60):
        """Send one command. The board's telnetd only honours CRLF."""
        self.seq += 1
        tag = "__B%04d__" % self.seq
        sender = cmd + "\r\n"
        self.buf = b""
        self.sock.sendall(sender.encode())
        time.sleep(0.15)
        self.sock.sendall(("echo " + tag + "\r\n").encode())
        end = time.time() + timeout
        needle = tag.encode()
        while time.time() < end:
            self._drain(0.4)
            if needle in self.buf:
                time.sleep(0.15)
                self._drain(0.25)
                break
        text = self._clean(self.buf)

        # Strip: the echoed command, the marker, and the prompt leftovers.
        text = text.replace("~ # ", "")
        lines = text.split("\n")
        out = []
        for line in lines:
            s = line.strip()
            if s == tag or s == "echo " + tag or s == cmd.strip():
                continue
            if re.fullmatch(r"echo __B\d{4}__", s):
                continue
            if s in ("~ #", "#"):
                continue
            out.append(line)
        return "\n".join(out).strip("\n")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # ---------- file transfer ----------
    def put(self, local, remote):
        """Chunked base64: robust on a 115200-ish console link."""
        size = os.path.getsize(local)
        # make sure target dir exists
        self.run("mkdir -p %s" % os.path.dirname(remote) or "/tmp")
        self.run(": > %s" % remote)
        chunk = 512
        offset = 0
        with open(local, "rb") as f:
            while True:
                blob = f.read(chunk)
                if not blob:
                    break
                b64 = base64.b64encode(blob).decode()
                self.run('printf %%s "%s" | base64 -d >> %s' % (b64, remote), timeout=30)
                offset += len(blob)
                pct = 100.0 * offset / size
                sys.stderr.write("\r  %d/%d (%.0f%%)" % (offset, size, pct))
                sys.stderr.flush()
        sys.stderr.write("\n")
        got = self.run("wc -c < %s" % remote).strip().split()
        return int(got[-1]) if got and got[-1].isdigit() else -1


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    mode = sys.argv[1]
    b = Board()
    try:
        if mode == "run":
            for cmd in sys.argv[2:]:
                print("$ " + cmd)
                print(b.run(cmd).rstrip())
        elif mode == "raw":
            b.sock.sendall(sys.argv[2].encode())
            time.sleep(0.5)
            b._drain(0.6)
            print(b._clean(b.buf))
        elif mode == "put":
            n = b.put(sys.argv[2], sys.argv[3])
            print("wrote %d bytes -> %s" % (n, sys.argv[3]))
        elif mode == "get":
            b.run("base64 %s > /tmp/.b64 && echo OK" % sys.argv[2])
            data = b.run("cat /tmp/.b64; rm -f /tmp/.b64")
            print(data)
        else:
            print("unknown mode")
            return 1
    finally:
        b.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
