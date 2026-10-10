#!/usr/bin/env python3
"""Monitor en vivo de la PS3 mientras se juega.

Lee por red, sin instalar nada en la consola:
- temperaturas de CPU y RSX y ventilador, de webMAN MOD (/cpursx.ps3);
- la traza de rendimiento de Cell64 (perf_trace.csv), que crece una línea por
  cada 60 VI; solo se descargan los bytes nuevos (FTP con REST).

Imprime una línea por ventana: velocidad, cuadros por segundo y en qué se fue
el tiempo del PPE (ms por segundo de juego), más los underruns de audio nuevos.

  tools/ps3-live.py [--trace /dev_hdd0/cell64/perf_trace.csv] [--every 2] [--history 3]

Columnas: VI inicial, velocidad, cps medidos, y ms por segundo de juego de
CPU, RSP gráfico, RSP de audio, sueño del limitador, texturas y espera del
RSX; xrun = underruns de audio nuevos. "<" marca ventanas bajo 99 %.

PS3_IP sale del entorno o de tools/ps3.conf (ignorado por git).
"""
import argparse
import csv
import ftplib
import io
import os
import re
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def ps3_ip():
    ip = os.environ.get("PS3_IP")
    conf = os.path.join(ROOT, "tools", "ps3.conf")
    if not ip and os.path.exists(conf):
        for line in open(conf):
            if line.startswith("PS3_IP="):
                ip = line.split("=", 1)[1].strip()
    if not ip:
        sys.exit("definir PS3_IP (o tools/ps3.conf)")
    return ip


def temps(ip):
    try:
        html = urllib.request.urlopen(f"http://{ip}/cpursx.ps3", timeout=3).read().decode("utf-8", "replace")
    except OSError:
        return "temp ?"
    cpu = re.search(r"CPU:\s*(\d+)°C", html)
    rsx = re.search(r"RSX:\s*(\d+)°C", html)
    fan = re.search(r"FAN:\s*(\d+)%", html)
    return "CPU {}° RSX {}° fan {}%".format(*(m.group(1) if m else "?" for m in (cpu, rsx, fan)))


def fetch_new(ip, path, offset):
    buf = io.BytesIO()
    with ftplib.FTP(ip, timeout=5) as ftp:
        ftp.login()
        try:
            size = ftp.size(path)
        except ftplib.error_perm:
            return b"", 0
        if size is None or size < offset:
            offset = 0  # archivo nuevo: otra sesión de Cell64
        if size == offset:
            return b"", offset
        ftp.retrbinary(f"RETR {path}", buf.write, rest=offset)
    return buf.getvalue(), offset


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--trace", default="/dev_hdd0/cell64/perf_trace.csv")
    ap.add_argument("--every", type=float, default=2.0, help="segundos entre consultas")
    ap.add_argument("--history", type=int, default=3, help="ventanas ya escritas que se muestran al arrancar")
    args = ap.parse_args()
    ip = ps3_ip()

    offset, header, pending, prev_under, first = 0, None, "", None, True
    print(f"PS3 {ip}, traza {args.trace}. Ctrl+C para salir.")
    print(f"{'VI':>6} {'vel':>5} {'cps':>5} {'cpu':>5} {'rspG':>5} {'rspA':>5} {'dorm':>5} {'tex':>4} {'rsxW':>5} {'xrun':>4}  temperaturas")
    try:
        while True:
            try:
                data, start = fetch_new(ip, args.trace, offset)
            except (OSError, EOFError, ftplib.Error) as e:
                print(f"(sin conexión: {e})")
                time.sleep(args.every)
                continue
            if start == 0 and offset:
                header, pending, prev_under = None, "", None
                print("(traza nueva)")
            offset = start + len(data)
            pending += data.decode("utf-8", "replace")
            lines = pending.split("\n")
            pending = lines.pop()
            new_rows = []
            for line in lines:
                if not line or line.startswith("#"):
                    continue
                row = next(csv.reader([line]))
                if row[0] == "kind":
                    header = row
                elif header and row[0] == "window":
                    new_rows.append(dict(zip(header, row)))
            if first and new_rows:
                skipped = new_rows[:-args.history] if args.history else new_rows
                if skipped:
                    prev_under = int(skipped[-1].get("audio_underruns", 0) or 0)
                new_rows = new_rows[len(skipped):]
            first = False
            t = temps(ip) if new_rows else None
            for r in new_rows:
                ms = lambda k: int(float(r.get(k, 0) or 0)) // 1000
                under = int(r.get("audio_underruns", 0) or 0)
                xrun = under - prev_under if prev_under is not None else 0
                prev_under = under
                vel = int(r["speed_permille"]) / 10
                flag = " <" if vel < 99 else ""
                print(f"{r['vi_first']:>6} {vel:>4.0f}% {float(r['measured_fps']):>5.1f} "
                      f"{ms('cpu_other_us'):>5} {ms('rsp_gfx_us'):>5} {ms('rsp_audio_us'):>5} "
                      f"{ms('limit_sleep_us'):>5} {ms('texture_update_us'):>4} {ms('rsx_wait_us'):>5} "
                      f"{xrun:>4}  {t}{flag}")
            time.sleep(args.every)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
