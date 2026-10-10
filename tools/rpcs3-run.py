#!/usr/bin/env python3
"""Corre un programa de Salsa64 en RPCS3 y espera su línea RESULTADO.

  tools/rpcs3-run.py rsx-triangle            (o la ruta a un .elf)
  tools/rpcs3-run.py jit-ppe --relaunch 10 --config tools/rpcs3/salsa64-interp.yml
  tools/rpcs3-run.py build/salsa64/salsa64.elf --embedded vitest

Con --embedded, el frontend usa esa ROM de prueba embebida (escribe
dev_hdd0/salsa64/embedded en RPCS3); con --rom NOMBRE, ese archivo de
dev_hdd0/salsa64 (sin pisar rom.z64); sin opciones, se borra.

Lanza RPCS3 con --no-gui y tools/rpcs3/salsa64.yml, crea el archivo de modo
autotest (src/platform/ps3/autotest.h) para que el programa salga solo, sigue
TTY.log e imprime las líneas del programa. RPCS3 se cierra por su cuenta al
terminar el proceso ("Exit RPCS3 when process finishes"); solo se mata con
SIGKILL si se cuelga (SIGTERM durante el cierre lo hace abortar). Si RPCS3
deja una ventana de error fatal (proceso `rpcs3 --error`), se registra su
mensaje y se cierra. Copia de RPCS3.log en build/logs/rpcs3-<programa>.rpcs3.log.
Código de salida: 0 si hubo RESULTADO sin FALLO, 1 si FALLO, 2 si no hubo
RESULTADO.
RPCS3 no es concluyente para rendimiento ni para el JIT (ver docs/proyecto.md).
"""
import argparse
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
RPCS3 = os.environ.get("RPCS3", "/Applications/RPCS3.app/Contents/MacOS/rpcs3")
LOGDIR = pathlib.Path.home() / "Library/Caches/rpcs3"
AUTOTEST_FLAG = pathlib.Path.home() / "Library/Application Support/rpcs3/dev_hdd0/salsa64/autotest"
# RPCS3 no ejecuta código generado: el recompilador se apaga con este archivo
# (src/platform/ps3/rec_cache.h). En la PS3 real no existe.
NOREC_FLAG = AUTOTEST_FLAG.parent / "sin-recompilador"
EMBED_FILE = AUTOTEST_FLAG.parent / "embedded"
CONFIG = ROOT / "tools/rpcs3/salsa64.yml"
EXIT_GRACE = 30  # segundos para que RPCS3 se cierre solo tras RESULTADO


def resolve_elf(arg: str) -> pathlib.Path:
    p = pathlib.Path(arg)
    if p.suffix == ".elf" and p.exists():
        return p.resolve()
    p = ROOT / "build" / f"salsa64-{arg}" / f"salsa64-{arg}.elf"
    if not p.exists():
        sys.exit(f"no existe {p}: compilar con tools/build.sh")
    return p


def close_error_dialogs(wait: float = 3.0) -> None:
    """Registra y cierra las ventanas de error fatal de RPCS3 (procesos
    `rpcs3 --error`), que pueden aparecer un poco después de la caída."""
    deadline = time.monotonic() + wait
    while True:
        ps = subprocess.run(["ps", "-axo", "pid=,args="], capture_output=True, text=True).stdout
        for row in ps.splitlines():
            pid, _, args = row.strip().partition(" ")
            if args.startswith(RPCS3 + " ") and " --error" in args:
                print(f"== RPCS3 mostró un error fatal: {args.split('--error', 1)[1].strip()[:400]}",
                      file=sys.stderr)
                os.kill(int(pid), signal.SIGKILL)
        if time.monotonic() >= deadline:
            return
        time.sleep(0.5)


def run_once(elf: pathlib.Path, config: pathlib.Path, timeout: float, keep_lines: list) -> str:
    """Devuelve 'resultado', 'salio' (terminó sin RESULTADO), 'fatal' o 'timeout'."""
    AUTOTEST_FLAG.parent.mkdir(parents=True, exist_ok=True)
    AUTOTEST_FLAG.touch()
    NOREC_FLAG.touch()
    tty = LOGDIR / "TTY.log"
    rlog = LOGDIR / "RPCS3.log"
    for old in (tty, rlog):  # el de la corrida anterior podría tener un fatal
        if old.exists():
            old.unlink()
    out = ROOT / "build/logs/rpcs3-stdout.log"
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w") as fout:
        proc = subprocess.Popen([RPCS3, "--no-gui", "--config", str(config), str(elf)],
                                stdout=fout, stderr=subprocess.STDOUT)
    pos = 0
    log_pos = 0
    deadline = time.monotonic() + timeout
    found_at = None
    status = "timeout"
    try:
        while time.monotonic() < deadline:
            if tty.exists():
                with open(tty, "rb") as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos += len(chunk)
                for line in chunk.decode("utf-8", "replace").splitlines():
                    line = line.strip("\x00")
                    if not line:
                        continue
                    print(line, flush=True)
                    keep_lines.append(line)
                    if "RESULTADO" in line and found_at is None:
                        found_at = time.monotonic()
            # Tras un error fatal RPCS3 queda detenido: no esperar al timeout.
            if found_at is None and rlog.exists() and rlog.stat().st_size > log_pos:
                with open(rlog, "rb") as f:
                    f.seek(log_pos)
                    chunk = f.read()
                    log_pos += len(chunk)
                if b"\xc2\xb7F " in chunk:
                    time.sleep(1)
                    status = "fatal"
                    break
            if found_at is not None and time.monotonic() - found_at > EXIT_GRACE:
                print(f"== RPCS3 no se cerró {EXIT_GRACE} s después del RESULTADO", file=sys.stderr)
                status = "resultado"
                break
            if proc.poll() is not None:
                status = "resultado" if found_at is not None else "salio"
                break
            time.sleep(0.2)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        close_error_dialogs(3.0 if status != "resultado" else 0.0)
        name = elf.stem.removeprefix("salsa64-")
        if (LOGDIR / "RPCS3.log").exists():
            shutil.copy(LOGDIR / "RPCS3.log", ROOT / f"build/logs/rpcs3-{name}.rpcs3.log")
    if status == "salio":
        # En el primer lanzamiento de un ELF nuevo (RPCS3 compila sus
        # módulos) a veces no se alcanza a leer el TTY.log; RPCS3.log trae al
        # final todo el TTY acumulado.
        tty_lines = accumulated_tty(rlog)
        if any("RESULTADO" in l for l in tty_lines):
            print("== TTY tomado del final de RPCS3.log", file=sys.stderr)
            for line in tty_lines:
                print(line, flush=True)
                keep_lines.append(line)
            status = "resultado"
    return status


def accumulated_tty(log: pathlib.Path) -> list:
    """Líneas de la sección "Accumulated RPCS3 TTY" de RPCS3.log."""
    if not log.exists():
        return []
    lines = log.read_text("utf-8", "replace").splitlines()
    try:
        start = lines.index("Accumulated RPCS3 TTY:") + 1
    except ValueError:
        return []
    out = []
    for line in lines[start:]:
        if line.startswith("End RPCS3 TTY Section"):
            break
        if line.strip("\x00"):
            out.append(line.strip("\x00"))
    return out


def show_rpcs3_errors() -> None:
    log = LOGDIR / "RPCS3.log"
    if not log.exists():
        return
    lines = [l for l in log.read_text("utf-8", "replace").splitlines()
             if "·F " in l or "·E " in l]
    if lines:
        print("--- errores de RPCS3.log (últimos 15):", file=sys.stderr)
        for l in lines[-15:]:
            print(l[:300], file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("program", help="nombre en tests/phase0 (p. ej. spu-compute) o ruta a .elf")
    ap.add_argument("--timeout", type=float, default=180,
                    help="segundos por intento (la primera vez RPCS3 compila los módulos del firmware)")
    ap.add_argument("--relaunch", type=int, default=0,
                    help="relanzar hasta N veces si termina sin RESULTADO")
    ap.add_argument("--config", type=pathlib.Path, default=CONFIG,
                    help="configuración de RPCS3 (por omisión tools/rpcs3/salsa64.yml)")
    ap.add_argument("--embedded", choices=["vitest", "gfxtest", "audiotest"],
                    help="ROM de prueba embebida que debe usar el frontend")
    ap.add_argument("--rom", help="archivo de dev_hdd0/salsa64 que debe cargar el frontend "
                    "(no se llama rom.z64 para no afectar a otras compilaciones)")
    ap.add_argument("--replay", help="volcado de la HLE (en dev_hdd0/salsa64) que el frontend "
                    "debe dibujar en el RSX (replay.c)")
    args = ap.parse_args()

    EMBED_FILE.parent.mkdir(parents=True, exist_ok=True)
    if args.replay:
        EMBED_FILE.write_text("dump:" + args.replay + "\n")
    elif args.rom:
        EMBED_FILE.write_text("file:" + args.rom + "\n")
    elif args.embedded:
        EMBED_FILE.write_text(args.embedded + "\n")
    elif EMBED_FILE.exists():
        EMBED_FILE.unlink()

    elf = resolve_elf(args.program)
    lines: list = []
    for attempt in range(args.relaunch + 1):
        print(f"== RPCS3 intento {attempt + 1}: {elf.relative_to(ROOT)}", file=sys.stderr)
        status = run_once(elf, args.config.resolve(), args.timeout, lines)
        if status == "resultado":
            break
        print(f"== terminó sin RESULTADO ({status})", file=sys.stderr)
        show_rpcs3_errors()
        if status == "timeout":
            break

    result = [l for l in lines if "RESULTADO" in l]
    if not result:
        return 2
    return 1 if "FALLO" in result[-1] else 0


if __name__ == "__main__":
    sys.exit(main())
