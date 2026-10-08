#!/usr/bin/env python3
"""A pretend Sunshine host: answers mDNS queries for _nvstream._tcp so the
client's discovery code can be tested without a real PC on the network."""
import socket
import struct
import sys
import time

SERVICE = "_nvstream._tcp.local"
INSTANCE = "TestHost._nvstream._tcp.local"
HOSTNAME = "testhost.local"
IP = "127.0.0.1"
PORT = 47989

TYPE_A, TYPE_PTR, TYPE_TXT, TYPE_SRV = 1, 12, 16, 33


def name(n):
    out = b""
    for lbl in n.split("."):
        out += bytes([len(lbl)]) + lbl.encode()
    return out + b"\x00"


def read_name(buf, off):
    labels = []
    jumped = False
    resume = off
    for _ in range(64):
        b = buf[off]
        if b == 0:
            off += 1
            break
        if b & 0xC0 == 0:
            n = b
            labels.append(buf[off + 1:off + 1 + n].decode("utf-8", "replace"))
            off += 1 + n
        else:
            ptr = ((b & 0x3F) << 8) | buf[off + 1]
            off += 2
            if not jumped:
                resume = off
                jumped = True
            off = ptr
    return (resume if jumped else off), ".".join(labels)


def parse_questions(query):
    count = struct.unpack(">H", query[4:6])[0]
    off = 12
    out = []
    for _ in range(count):
        off, qname = read_name(query, off)
        if off + 4 > len(query):
            break
        qtype = struct.unpack(">H", query[off:off + 2])[0]
        off += 4
        out.append((qname, qtype))
    return out


def rr(nm, rtype, rdata, ttl=120):
    return name(nm) + struct.pack(">HHIH", rtype, 1, ttl, len(rdata)) + rdata


def build(query):
    answers = b""
    additional = b""
    ancount = 0
    arcount = 0

    for qname, qtype in parse_questions(query):
        if qtype == TYPE_PTR:
            answers += rr(SERVICE, TYPE_PTR, name(INSTANCE))
            ancount += 1
            additional += rr(INSTANCE, TYPE_SRV,
                             struct.pack(">HHH", 0, 0, PORT) + name(HOSTNAME))
            additional += rr(INSTANCE, TYPE_TXT, b"\x03a=b")
            arcount += 2
        elif qtype == TYPE_SRV:
            answers += rr(INSTANCE, TYPE_SRV,
                          struct.pack(">HHH", 0, 0, PORT) + name(HOSTNAME))
            ancount += 1
            additional += rr(HOSTNAME, TYPE_A, socket.inet_aton(IP))
            arcount += 1
        elif qtype == TYPE_A:
            answers += rr(HOSTNAME, TYPE_A, socket.inet_aton(IP))
            ancount += 1

    if ancount == 0:
        return None
    header = query[0:2] + struct.pack(">HHHHH", 0x8400, 0, ancount, 0, arcount)
    return header + answers + additional


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except (AttributeError, OSError):
        pass
    sock.bind(("", 5353))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    sock.setsockopt(
        socket.IPPROTO_IP,
        socket.IP_ADD_MEMBERSHIP,
        socket.inet_aton("224.0.0.251") + socket.inet_aton("0.0.0.0"),
    )
    deadline = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 40)
    print("fake mDNS host up", flush=True)
    while time.time() < deadline:
        sock.settimeout(1.0)
        try:
            data, src = sock.recvfrom(9000)
        except socket.timeout:
            continue
        if src[1] != 5353:
            continue
        try:
            resp = build(data)
        except Exception as exc:
            print("skip:", exc, flush=True)
            continue
        if resp is None:
            continue
        print("answered", src, flush=True)
        sock.sendto(resp, ("224.0.0.251", 5353))


if __name__ == "__main__":
    main()
