#!/usr/bin/env python3
"""
Minimal HTTP/CONNECT proxy for the k50v1 board.

Why this exists: Windows Internet Connection Sharing is bound correctly on
this machine (WLAN = public, "Ethernet 5" = private) but neither its DHCP
allocator nor its NAT carries traffic. The board therefore points apk/apt at
this proxy: requests originate from a normal Windows process, which does have
working internet.

Usage (on the PC):
    python proxy.py 8888

On the board:
    export http_proxy=http://192.168.137.1:8888
    export https_proxy=http://192.168.137.1:8888
    apk update && apk add nano
"""
import select
import socket
import ssl
import sys
import threading
import time

BUFSZ = 65536
IDLE = 120
VERBOSE = True


def log(msg):
    if VERBOSE:
        print("%s %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def pump(a, b):
    """Shovel bytes both ways until either side closes."""
    socks = [a, b]
    try:
        while True:
            r, _, x = select.select(socks, [], socks, IDLE)
            if x or not r:
                return
            for s in r:
                data = s.recv(BUFSZ)
                if not data:
                    return
                (b if s is a else a).sendall(data)
    except OSError:
        pass


def read_head(sock):
    buf = b""
    while b"\r\n\r\n" not in buf:
        try:
            chunk = sock.recv(4096)
        except OSError:
            return None
        if not chunk:
            return None
        buf += chunk
        if len(buf) > 65536:
            return None
    return buf


def fail(client, code, text):
    """Always answer, even on error: a silent close shows up as
    'wget: error getting response' and tells us nothing."""
    body = text.encode()
    try:
        client.sendall(
            ("HTTP/1.1 %d %s\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n" % (code, text, len(body))).encode()
            + body
        )
    except OSError:
        pass


def handle(client, peer):
    upstream = None
    try:
        head = read_head(client)
        if not head:
            log("%s: empty request" % (peer,))
            return
        request_line = head.split(b"\r\n", 1)[0].decode("latin-1", "replace")
        log("%s: %s" % (peer, request_line))
        parts = request_line.split()
        if len(parts) < 2:
            fail(client, 400, "bad request line")
            return
        method, target = parts[0], parts[1]

        if method.upper() == "CONNECT":
            host, _, port = target.rpartition(":")
            if not host:
                fail(client, 400, "bad CONNECT target")
                return
            try:
                upstream = socket.create_connection((host, int(port or 443)), 15)
            except OSError as exc:
                log("%s: upstream %s failed: %s" % (peer, target, exc))
                fail(client, 502, "cannot reach %s" % target)
                return
            client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            pump(client, upstream)
            return

        # Plain HTTP: turn the absolute URI into origin-form.
        if target.startswith("http://"):
            rest = target[7:]
            hostport, _, path = rest.partition("/")
            path = "/" + path
        elif target.startswith("https://"):
            """
            busybox wget does not do CONNECT: it hands the absolute https
            URL to the proxy in a plain request and expects the proxy to
            deal with TLS. Terminate TLS here and relay the plaintext --
            it is a point-to-point link over USB, so there is nobody to
            protect the hop from.
            """
            rest = target[8:]
            hostport, _, path = rest.partition("/")
            path = "/" + path
            host, _, port = hostport.partition(":")
            try:
                raw = socket.create_connection((host, int(port or 443)), 15)
                raw.settimeout(10)         # fail fast instead of hanging wget
                ctx = ssl.create_default_context()
                upstream = ctx.wrap_socket(raw, server_hostname=host)
            except (OSError, ssl.SSLError) as exc:
                log("%s: tls to %s failed: %s" % (peer, hostport, exc))
                fail(client, 502, "cannot reach %s" % hostport)
                return
            upstream.sendall(
                method.encode() + b" " + path.encode() + b" HTTP/1.1\r\n"
                + b"\r\n".join(head.split(b"\r\n")[1:])
            )
            pump(client, upstream)
            return
        else:
            hostport = ""
            path = target
            for line in head.split(b"\r\n"):
                low = line.lower()
                if low.startswith(b"host:"):
                    hostport = line.split(b":", 1)[1].strip().decode("latin-1")
                    break
        if not hostport:
            fail(client, 400, "no Host header")
            return
        host, _, port = hostport.partition(":")
        try:
            upstream = socket.create_connection((host, int(port or 80)), 15)
        except OSError as exc:
            log("%s: upstream %s failed: %s" % (peer, hostport, exc))
            fail(client, 502, "cannot reach %s" % hostport)
            return
        upstream.sendall(
            method.encode() + b" " + path.encode() + b" HTTP/1.1\r\n"
            + b"\r\n".join(head.split(b"\r\n")[1:])
        )
        pump(client, upstream)
    except Exception as exc:                     # never die on one client
        log("%s: exception %r" % (peer, exc))
        fail(client, 500, "proxy error")
    finally:
        for s in (client, upstream):
            if s is not None:
                try:
                    s.close()
                except OSError:
                    pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8888
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(64)
    log("proxy listening on 0.0.0.0:%d" % port)
    while True:
        client, peer = srv.accept()
        log("accept from %s:%d" % peer)
        threading.Thread(target=handle, args=(client, peer), daemon=True).start()


if __name__ == "__main__":
    main()
