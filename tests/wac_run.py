"""WAC tactical suite runner via UCI. Usage: python tests/wac_run.py [depth] [max_pos]"""
import subprocess, sys, time, re
import chess

ENGINE = r"output\main.exe"
EPD = r"tests\wac.epd"
DEPTH = int(sys.argv[1]) if len(sys.argv) > 1 else 10
MAXPOS = int(sys.argv[2]) if len(sys.argv) > 2 else 0
ONLYIDS = set()
if len(sys.argv) > 3:
    ONLYIDS = set(l.strip() for l in open(sys.argv[3]) if l.strip())


def parse_epd(line):
    m = re.search(r"bm\s+([^;]+);", line)
    bm = m.group(1).strip() if m else ""
    mid = re.search(r'id\s+"([^"]+)"', line)
    pid = mid.group(1) if mid else "?"
    fen = line.split(" bm ")[0].strip()
    return fen, bm, pid


def main():
    lines = [l.strip() for l in open(EPD) if l.strip()]
    if MAXPOS:
        lines = lines[:MAXPOS]
    if ONLYIDS:
        keep = []
        for l in lines:
            _, _, pid = parse_epd(l)
            if pid in ONLYIDS:
                keep.append(l)
        lines = keep
        print(f"Running {len(lines)} selected positions")
    eng = subprocess.Popen([ENGINE], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, text=True, bufsize=1)
    out = eng.stdout

    def send(cmd):
        eng.stdin.write(cmd + "\n")
        eng.stdin.flush()

    def expect_bestmove():
        while True:
            line = out.readline()
            if not line:
                return None
            if line.startswith("bestmove"):
                parts = line.split()
                return parts[1] if len(parts) > 1 else None

    send("uci")
    while True:
        line = out.readline()
        if "uciok" in line:
            break
    send("setoption name Threads value 1")
    send("setoption name Hash value 64")
    send("isready")
    while True:
        line = out.readline()
        if "readyok" in line:
            break

    solved = 0
    total = 0
    fails = []
    t0 = time.time()
    for line in lines:
        fen, bm_san, pid = parse_epd(line)
        total += 1
        try:
            board = chess.Board(fen)
            exp_list = [board.parse_san(s).uci() for s in bm_san.split()]
        except Exception as e:
            print(f"{pid}: BAD EPD ({e})")
            continue
        send(f"position fen {fen}")
        send(f"go depth {DEPTH}")
        got = expect_bestmove()
        ok = (got in exp_list)
        solved += ok
        if not ok:
            fails.append((pid, bm_san, "/".join(exp_list), got))
    send("quit")
    eng.wait(timeout=10)
    dt = time.time() - t0
    print(f"\nWAC depth={DEPTH}: {solved}/{total} solved in {dt:.0f}s")
    with open(r"tests\wac_miss.txt", "w") as f:
        for pid, san, exp, got in fails:
            f.write(f"{pid}\n")
    for pid, san, exp, got in fails[:40]:
        print(f"  MISS {pid}: expected {san} ({exp}), got {got}")
    if len(fails) > 40:
        print(f"  ... +{len(fails) - 40} more misses")


main()
