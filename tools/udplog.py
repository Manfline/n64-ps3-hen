#!/usr/bin/env python3
"""Receptor del logger UDP de Cell64 (src/platform/ps3/log.c).

Escucha en 0.0.0.0:18194 (broadcast o unicast desde la PS3), imprime cada
línea con la IP de origen y, con --out, la agrega a un archivo.

  tools/udplog.py [--port 18194] [--out build/logs/ps3.log] [--until TEXTO]

--until termina (código 0) al recibir una línea que contenga TEXTO; útil
para automatizar pruebas: p. ej. --until "RESULTADO".
"""
import argparse
import datetime
import socket
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=18194)
    ap.add_argument("--out", help="archivo donde agregar las líneas")
    ap.add_argument("--until", help="salir al recibir una línea con este texto")
    ap.add_argument("--timeout", type=float, help="segundos sin datos antes de salir con error")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if hasattr(socket, "SO_REUSEPORT"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind(("0.0.0.0", args.port))
    if args.timeout:
        sock.settimeout(args.timeout)

    out = open(args.out, "a", encoding="utf-8") if args.out else None
    print(f"escuchando UDP en 0.0.0.0:{args.port}", file=sys.stderr)
    try:
        while True:
            try:
                data, (host, _) = sock.recvfrom(65535)
            except socket.timeout:
                print("tiempo de espera agotado", file=sys.stderr)
                return 1
            stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            for line in data.decode("utf-8", "replace").splitlines():
                text = f"{stamp} {host} {line}"
                print(text, flush=True)
                if out:
                    out.write(text + "\n")
                    out.flush()
                if args.until and args.until in line:
                    return 0
    except KeyboardInterrupt:
        return 0
    finally:
        if out:
            out.close()


if __name__ == "__main__":
    sys.exit(main())
