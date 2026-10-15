"""Full SPRT for GC-Chess-Engine - pure Python, no cutechess needed.

Mirrors run_sprt.bat defaults: TC=10+0.1, elo0=0, elo1=5, alpha=beta=0.05,
draw adjudication ~50/8/10, resign ~1000cp/3, Hash=64, Threads=1.

Runs anywhere python-chess installs: WSL, Linux, Windows, Kaggle notebook.

QUICK START (WSL / Linux):
    pip install python-chess
    make clean && make -j$(nproc)                 # builds output/main
    cp output/main output/main_baseline          # baseline BEFORE your patch,
                                                 # or: git stash && make && cp ... && git stash pop && make
    python tests/sprt_run.py --games 2000 --tc 10+0.1 --concurrency 4

KAGGLE NOTEBOOK (cells):
    !pip install -q python-chess
    !git clone <your-repo-url> && cd GC-Chess_Engine
    !make -j4                                     # output/main
    !cp output/main output/main_new
    !git stash -q && make -j4 && cp output/main output/main_baseline && git stash pop -q && make -j4
    !python tests/sprt_run.py --new output/main_new --base output/main_baseline \\
        --games 2000 --tc 10+0.1 --concurrency 2 --pgn tests/sprt_results.pgn

EXIT CODES: 0 = H1 accepted (PASS, engine improved),
            1 = H0 accepted (FAIL, no improvement),
            2 = undecided (hit --games cap), 3 = usage/runtime error.
PGN is appended incrementally, so a preempted Kaggle run keeps its games.
Time-forfeits/crashes are counted separately and WARNED about loudly:
any forfeit on SPRT = time-management bug, do NOT merge before investigating.
"""
import argparse
import concurrent.futures
import datetime
import math
import os
import random
import subprocess
import sys
import threading
import time

try:
    import chess
    import chess.engine
    import chess.pgn
except ImportError:
    sys.exit("ERROR: python-chess is required:  pip install python-chess")


# ---------------------------------------------------------------- openings ---
# Balanced ECO lines as SAN move lists from the start position (always legal,
# colours alternate per pair). Used when --openings is not given.
BUILTIN_BOOK = [
    ["e4", "e5", "Nf3", "Nc6", "Bb5", "a6", "Ba4", "Nf6"],
    ["e4", "e5", "Nf3", "Nc6", "Bc4", "Bc5", "c3", "Nf6"],
    ["e4", "c5", "Nf3", "d6", "d4", "cxd4", "Nxd4", "Nf6"],
    ["e4", "c5", "Nf3", "Nc6", "d4", "cxd4", "Nxd4", "g6"],
    ["e4", "e6", "d4", "d5", "Nc3", "Nf6", "e5", "Nfd7"],
    ["e4", "e6", "d4", "d5", "Nd2", "c5", "exd5", "Qxd5"],
    ["e4", "c6", "d4", "d5", "exd5", "cxd5", "c4", "Nf6"],
    ["e4", "c6", "d4", "d5", "Nc3", "dxe4", "Nxe4", "Bf5"],
    ["e4", "d5", "exd5", "Qxd5", "Nc3", "Qa5", "d4", "Nf6"],
    ["e4", "d6", "d4", "Nf6", "Nc3", "g6", "Nf3", "Bg7"],
    ["e4", "g6", "d4", "Bg7", "Nc3", "d6", "Nf3", "Nf6"],
    ["e4", "e5", "Nf3", "Nc6", "d4", "exd4", "Nxd4", "Nf6"],
    ["d4", "d5", "c4", "e6", "Nc3", "Nf6", "Bg5", "Be7"],
    ["d4", "d5", "c4", "c6", "Nf3", "Nf6", "Nc3", "dxc4"],
    ["d4", "Nf6", "c4", "e6", "Nc3", "Bb4", "e3", "O-O"],
    ["d4", "Nf6", "c4", "g6", "Nc3", "Bg7", "e4", "d6"],
    ["d4", "f5", "g3", "Nf6", "Bg2", "e6", "c4", "d5"],
    ["d4", "d5", "Bf4", "Nf6", "e3", "e6", "Nf3", "c5"],
    ["c4", "e5", "Nc3", "Nf6", "Nf3", "Nc6", "g3", "d5"],
    ["c4", "Nf6", "Nc3", "e6", "Nf3", "b6", "g3", "Bb7"],
    ["Nf3", "d5", "g3", "Nf6", "Bg2", "e6", "O-O", "Be7"],
    ["e4", "c5", "c3", "Nf6", "e5", "Nd5", "d4", "cxd4"],
    ["d4", "Nf6", "Nf3", "d5", "c4", "e6", "Nc3", "Be7"],
    ["e4", "e5", "Bc4", "Nf6", "d3", "c6", "Nf3", "d5"],
]


def load_openings(path, book_ply):
    """Load openings from .epd (FEN per line) or .pgn (position at book_ply)."""
    if path is None:
        boards = []
        for line in BUILTIN_BOOK:
            b = chess.Board()
            for san in line:
                b.push_san(san)
            boards.append(b)
        return boards
    if path.lower().endswith(".epd"):
        boards = []
        with open(path) as f:
            for raw in f:
                raw = raw.strip()
                if not raw:
                    continue
                fen = raw.split(" bm ")[0].split(" ce ")[0].strip()
                boards.append(chess.Board(fen))
        if not boards:
            sys.exit(f"ERROR: no positions found in {path}")
        return boards
    # assume PGN
    boards = []
    with open(path) as f:
        while True:
            game = chess.pgn.read_game(f)
            if game is None:
                break
            b = game.board()
            for i, mv in enumerate(game.mainline_moves()):
                if i >= book_ply:
                    break
                b.push(mv)
            if len(list(b.move_stack)) >= 4:
                boards.append(b)
    if not boards:
        sys.exit(f"ERROR: no games/positions found in {path}")
    return boards


# ------------------------------------------------------------------ SPRT ----
class TrinomialSprt:
    """Sequential Probability Ratio Test, trinomial W/D/L model (fishtest).

    Probabilities under elo hypothesis E with draw_elo Dd (BayesElo model):
        P(W) = 1 / (1 + 10**((Dd - E)/400))
        P(L) = 1 / (1 + 10**((Dd + E)/400))
        P(D) = 1 - P(W) - P(L)
    draw_elo is re-estimated from the observed draw ratio after every game:
        Dd = 400 * log10((1 + d) / (1 - d)),  d = draws / games
    LLR accumulates log(P1/P0) per game. H0 accepted if LLR <= lower,
    H1 accepted if LLR >= upper, with
        lower = log(beta / (1 - alpha)), upper = log((1 - beta) / alpha).
    """

    def __init__(self, elo0=0.0, elo1=5.0, alpha=0.05, beta=0.05):
        self.elo0 = elo0
        self.elo1 = elo1
        self.lower = math.log(beta / (1.0 - alpha))
        self.upper = math.log((1.0 - beta) / alpha)
        self.wins = 0      # from the perspective of --new
        self.losses = 0
        self.draws = 0
        self.llr = 0.0

    @staticmethod
    def _probs(elo, draw_elo):
        eps = 1e-12
        pw = 1.0 / (1.0 + 10.0 ** ((draw_elo - elo) / 400.0))
        pl = 1.0 / (1.0 + 10.0 ** ((draw_elo + elo) / 400.0))
        pd = max(1.0 - pw - pl, eps)
        return max(pw, eps), max(pl, eps), pd

    def update(self, result):
        """result: 'win' | 'loss' | 'draw' (from --new perspective)."""
        if result == "win":
            self.wins += 1
        elif result == "loss":
            self.losses += 1
        else:
            self.draws += 1
        n = self.games
        d = self.draws / n
        draw_elo = 400.0 * math.log10((1.0 + d) / max(1.0 - d, 1e-9))
        p0w, p0l, p0d = self._probs(self.elo0, draw_elo)
        p1w, p1l, p1d = self._probs(self.elo1, draw_elo)
        if result == "win":
            self.llr += math.log(p1w / p0w)
        elif result == "loss":
            self.llr += math.log(p1l / p0l)
        else:
            self.llr += math.log(p1d / p0d)

    @property
    def games(self):
        return self.wins + self.losses + self.draws

    def verdict(self):
        if self.games < 10:
            return None
        if self.llr >= self.upper:
            return "H1"
        if self.llr <= self.lower:
            return "H0"
        return None

    def elo(self):
        """Elo estimate + 2-sigma error (from --new perspective)."""
        n = self.games
        if n == 0:
            return 0.0, 0.0
        w, l, d = self.wins / n, self.losses / n, self.draws / n
        s = w + d / 2.0
        if s <= 0.0 or s >= 1.0:
            return 0.0, 0.0
        elo = -400.0 * math.log10(1.0 / s - 1.0)
        var = (w * (1.0 - s) ** 2 + l * s ** 2 + d * (0.5 - s) ** 2) / n
        se = math.sqrt(max(var, 0.0))
        elo_err = 2.0 * 400.0 * se / (s * (1.0 - s) * math.log(10.0))
        return elo, elo_err


# ------------------------------------------------------------------ game ----
RESIGN_CP = 1000        # like cutechess: -resign movecount=3 score=1000
RESIGN_MOVES = 3        # ... consecutive fullmoves (x2 halfmoves below)
RESIGN_MATE = 5         # mate-in-N or less, persisting, adjudicates
DRAW_CP = 10            # like cutechess: -draw movenumber=50 movecount=8 score=10
DRAW_PLIES = 16         # ... both sides within +/-DRAW_CP this many halfmoves
DRAW_MIN_PLY = 100      # ... only after fullmove 50
GRACE_S = 1.0           # wall-clock grace over (clock + inc) before forfeit


def white_cp(score):
    """Engine Score -> centipawns from White's perspective (mate -> +/-100000)."""
    if score is None:
        return 0
    w = score.white()
    if w.is_mate():
        m = w.mate()
        return 100000 if (m is not None and m > 0) else -100000
    cp = w.score()
    return 0 if cp is None else cp


def play_game(new_path, base_path, opening, new_is_white, opts, game_no):
    """Play one game. Returns (outcome, pgn_text, note).

    outcome is '1-0' | '0-1' | '1/2-1/2' (game-board perspective).
    note describes adjudication/termination, incl. forfeits.
    """
    board = opening.copy(stack=False)
    engines = {}
    clocks = {chess.WHITE: opts.tc_base, chess.BLACK: opts.tc_base}
    try:
        engines[chess.WHITE] = chess.engine.SimpleEngine.popen_uci(
            new_path if new_is_white else base_path)
        engines[chess.BLACK] = chess.engine.SimpleEngine.popen_uci(
            base_path if new_is_white else new_path)
        for eng in engines.values():
            eng.configure({"Hash": opts.hash, "Threads": opts.threads})
            for kv in opts.engine_option:
                name, _, value = kv.partition("=")
                eng.configure({name.strip(): value.strip()})
        for eng in engines.values():
            try:
                eng.protocol.ucinewgame()
            except (chess.engine.EngineError, AttributeError):
                pass

        game = chess.pgn.Game.from_board(board)
        game.headers["White"] = "New" if new_is_white else "Baseline"
        game.headers["Black"] = "Baseline" if new_is_white else "New"
        game.headers["Date"] = datetime.date.today().isoformat()
        game.headers["Round"] = str(game_no)
        if opts.tc_mode == "tc":
            game.headers["TimeControl"] = f"{opts.tc_base}+{opts.tc_inc}"
        node = game.end()

        bad_cp_run = 0       # consecutive halfmoves side-to-move is losing badly
        mate_run = 0         # consecutive halfmoves with short forced mate on board
        mate_sign = 0
        low_run = 0          # consecutive halfmoves with |cp| <= DRAW_CP
        loser_pov = 0
        result = None
        note = ""

        while True:
            stm = board.turn
            eng = engines[stm]
            if opts.tc_mode == "tc":
                limit = chess.engine.Limit(
                    white_clock=max(clocks[chess.WHITE], 0.05),
                    black_clock=max(clocks[chess.BLACK], 0.05),
                    white_inc=opts.tc_inc, black_inc=opts.tc_inc)
            elif opts.tc_mode == "movetime":
                limit = chess.engine.Limit(time=opts.movetime / 1000.0)
            else:
                limit = chess.engine.Limit(depth=opts.depth)

            t0 = time.monotonic()
            clock_before = clocks[stm]
            try:
                res = eng.play(board, limit)
            except (chess.engine.EngineTerminatedError,
                    chess.engine.EngineError) as e:
                winner = "1-0" if stm == chess.BLACK else "0-1"
                return winner, "", f"ENGINE CRASH ({game.headers['White' if stm == chess.WHITE else 'Black']}): {e}"
            elapsed = time.monotonic() - t0

            if opts.tc_mode == "tc":
                clocks[stm] = clock_before - elapsed + opts.tc_inc
                overstep = elapsed - (clock_before + opts.tc_inc)
                if overstep > GRACE_S:
                    winner = "1-0" if stm == chess.BLACK else "0-1"
                    side = "White" if stm == chess.WHITE else "Black"
                    exporter = chess.pgn.StringExporter(
                        headers=True, variations=True, comments=True)
                    return (winner, game.accept(exporter),
                            f"TIME FORFEIT by {side} "
                            f"(used {elapsed:.2f}s with {clock_before:.2f}s+{opts.tc_inc}s)")

            if res.move is None:
                winner = "1-0" if stm == chess.BLACK else "0-1"
                return winner, "", "NO MOVE (engine returned none)"
            if res.move not in board.legal_moves:
                winner = "1-0" if stm == chess.BLACK else "0-1"
                return winner, "", f"ILLEGAL MOVE {res.move.uci()}"

            info_score = white_cp(res.info.get("score")) if res.info else 0
            board.push(res.move)
            node = node.add_variation(res.move)

            # --- adjudication by score (White perspective) ---
            if info_score >= RESIGN_CP or info_score <= -RESIGN_CP:
                if (info_score >= RESIGN_CP and loser_pov >= 0) or \
                   (info_score <= -RESIGN_CP and loser_pov <= 0):
                    bad_cp_run = bad_cp_run + 1 if (
                        (info_score >= RESIGN_CP) == (loser_pov >= 0) or bad_cp_run == 0) else 1
                loser_pov = 1 if info_score >= RESIGN_CP else -1
            else:
                bad_cp_run = 0
                loser_pov = 0
            if bad_cp_run >= RESIGN_MOVES * 2:
                result = "1-0" if loser_pov > 0 else "0-1"
                note = f"adjudicated: cp beyond +/-{RESIGN_CP} for {RESIGN_MOVES} moves"
                break

            mate_pov = 0
            if res.info and res.info.get("score") is not None:
                w = res.info["score"].white()
                if w.is_mate() and w.mate() is not None and abs(w.mate()) <= RESIGN_MATE:
                    mate_pov = 1 if w.mate() > 0 else -1
            if mate_pov != 0 and (mate_run == 0 or mate_sign == mate_pov):
                mate_run += 1
                mate_sign = mate_pov
            else:
                mate_run = 1 if mate_pov != 0 else 0
                mate_sign = mate_pov
            if mate_run >= RESIGN_MOVES * 2:
                result = "1-0" if mate_sign > 0 else "0-1"
                note = f"adjudicated: forced mate <= {RESIGN_MATE} persisted"
                break

            if abs(info_score) <= DRAW_CP:
                low_run += 1
            else:
                low_run = 0
            if board.ply() >= DRAW_MIN_PLY and low_run >= DRAW_PLIES:
                result = "1/2-1/2"
                note = (f"adjudicated: both within +/-{DRAW_CP}cp for "
                        f"{DRAW_PLIES // 2} moves")
                break

            # --- natural terminations ---
            if board.is_checkmate():
                result = "1-0" if board.turn == chess.BLACK else "0-1"
                note = "checkmate"
                break
            if (board.is_stalemate() or board.is_insufficient_material() or
                    board.is_fivefold_repetition() or board.is_seventyfive_moves()):
                result = "1/2-1/2"
                note = "draw by rule (stalemate/material/5fold/75move)"
                break
            if board.is_repetition(3) or board.is_fifty_moves():
                result = "1/2-1/2"
                note = "draw claimed (3fold/50move)"
                break
            if board.ply() - opening.ply() >= opts.max_plies:
                result = "1/2-1/2"
                note = f"draw: max plies ({opts.max_plies}) reached"
                break

        game.headers["Result"] = result
        game.headers["Termination"] = note
        exporter = chess.pgn.StringExporter(headers=True,
                                            variations=False, comments=False)
        return result, game.accept(exporter), note
    finally:
        for eng in engines.values():
            try:
                eng.quit()
            except Exception:
                pass


# ------------------------------------------------------------------- main ---
def parse_tc(s):
    base, _, inc = s.partition("+")
    return float(base), float(inc)


def main():
    ap = argparse.ArgumentParser(description="Full SPRT for GC-Chess-Engine (no cutechess needed).")
    exe = ".exe" if os.name == "nt" else ""
    ap.add_argument("--new", default=f"output/main{exe}")
    ap.add_argument("--base", default=f"output/main_baseline{exe}")
    ap.add_argument("--games", type=int, default=2000)
    ap.add_argument("--tc", default="10+0.1",
                    help="base+inc seconds, e.g. 10+0.1 (mirrors run_sprt.bat)")
    ap.add_argument("--movetime", type=float, default=None,
                    help="fixed ms per move instead of TC")
    ap.add_argument("--depth", type=int, default=None,
                    help="fixed depth per move instead of TC")
    ap.add_argument("--concurrency", type=int, default=2)
    ap.add_argument("--elo0", type=float, default=0.0)
    ap.add_argument("--elo1", type=float, default=5.0)
    ap.add_argument("--alpha", type=float, default=0.05)
    ap.add_argument("--beta", type=float, default=0.05)
    ap.add_argument("--hash", type=int, default=64)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--engine-option", action="append", default=[],
                    help="extra UCI option, repeatable: --engine-option Contempt=0")
    ap.add_argument("--openings", default=None,
                    help=".epd FEN list or .pgn book (default: 24 built-in lines)")
    ap.add_argument("--book-ply", type=int, default=10,
                    help="plies taken from each PGN book game")
    ap.add_argument("--max-plies", type=int, default=512)
    ap.add_argument("--pgn", default="tests/sprt_results.pgn")
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--selftest", action="store_true",
                    help="verify SPRT math on synthetic data, no engines needed")
    opts = ap.parse_args()

    if opts.selftest:
        return selftest()

    for p in (opts.new, opts.base):
        if not os.path.isfile(p):
            sys.exit(f"ERROR: engine not found: {p}")
    opts.tc_base, opts.tc_inc = 10.0, 0.1
    if opts.movetime is not None:
        opts.tc_mode = "movetime"
        opts.movetime = opts.movetime
    elif opts.depth is not None:
        opts.tc_mode = "depth"
    else:
        opts.tc_mode = "tc"
        opts.tc_base, opts.tc_inc = parse_tc(opts.tc)
    if opts.concurrency < 1:
        sys.exit("ERROR: --concurrency must be >= 1")

    openings = load_openings(opts.openings, opts.book_ply)
    rng = random.Random(opts.seed)
    order = list(range(len(openings)))
    rng.shuffle(order)
    openings = [openings[i] for i in order]
    print(f"SPRT: {opts.new} (New) vs {opts.base} (Baseline)", flush=True)
    print(f"  elo0={opts.elo0} elo1={opts.elo1} alpha={opts.alpha} beta={opts.beta} "
          f"max_games={opts.games} concurrency={opts.concurrency}", flush=True)
    print(f"  openings: {len(openings)} ({'builtin' if opts.openings is None else opts.openings})",
          flush=True)

    sprt = TrinomialSprt(opts.elo0, opts.elo1, opts.alpha, opts.beta)
    lock = threading.Lock()
    counter = [0]
    forfeits = [0]
    pgn_dir = os.path.dirname(opts.pgn)
    if pgn_dir:
        os.makedirs(pgn_dir, exist_ok=True)
    # fresh PGN file for this run
    open(opts.pgn, "w").close()

    def result_for_new(outcome, new_is_white):
        if outcome == "1/2-1/2":
            return "draw"
        new_won = (outcome == "1-0") == new_is_white
        return "win" if new_won else "loss"

    def worker():
        while True:
            with lock:
                if counter[0] >= opts.games or sprt.verdict() is not None:
                    return
                counter[0] += 1
                game_no = counter[0]
            pair = (game_no - 1) // 2
            new_is_white = (game_no % 2 == 1)
            opening = openings[pair % len(openings)]
            outcome, pgn_text, note = play_game(
                opts.new, opts.base, opening, new_is_white, opts, game_no)
            res = result_for_new(outcome, new_is_white)
            is_forfeit = ("FORFEIT" in note or "CRASH" in note or "ILLEGAL" in note
                          or "NO MOVE" in note)
            with lock:
                if is_forfeit:
                    forfeits[0] += 1
                sprt.update(res)
                elo, err = sprt.elo()
                with open(opts.pgn, "a") as f:
                    if pgn_text:
                        f.write(pgn_text + "\n\n")
                v = sprt.verdict()
                wdl = f"W{sprt.wins}-D{sprt.draws}-L{sprt.losses}"
                print(f"[{game_no:4d}] {outcome:7s} ({res:4s}) {wdl} "
                      f"elo {elo:+.1f}+/-{err:.1f} LLR {sprt.llr:+.2f} "
                      f"[{sprt.lower:+.2f},{sprt.upper:+.2f}] {note}",
                      flush=True)
                if v is not None:
                    print(f"SPRT verdict after {sprt.games} games: {v}",
                          flush=True)
                    return v
        return None

    with concurrent.futures.ThreadPoolExecutor(
            max_workers=opts.concurrency) as pool:
        futs = [pool.submit(worker) for _ in range(opts.concurrency)]
        verdicts = [f.result() for f in futs]
    verdict = next((v for v in verdicts if v is not None), None)

    elo, err = sprt.elo()
    print(f"\nFinal: {sprt.games} games W{sprt.wins}-D{sprt.draws}-L{sprt.losses} "
          f"elo {elo:+.1f}+/-{err:.1f} LLR {sprt.llr:+.2f}", flush=True)
    if forfeits[0]:
        print(f"*** {forfeits[0]} TIME-FORFEIT/CRASH occurrence(s) ***",
              flush=True)
    if verdict == "H1":
        print("SPRT RESULT: PASS - H1 accepted (engine improved).")
        return 0
    if verdict == "H0":
        print("SPRT RESULT: FAIL - H0 accepted (no improvement).")
        if forfeits[0]:
            print("WARNING: forfeits detected - do NOT merge before investigating.")
        return 1
    print("SPRT RESULT: UNDECIDED - hit game cap without crossing a bound.")
    if forfeits[0]:
        print("WARNING: forfeits detected - do NOT merge before investigating.")
    return 2


def selftest():
    """Verify SPRT math on synthetic score streams (no engines)."""
    # 1. bounds for alpha=beta=0.05
    s = TrinomialSprt(0.0, 5.0, 0.05, 0.05)
    assert abs(s.lower + 2.944438979) < 1e-6, s.lower
    assert abs(s.upper - 2.944438979) < 1e-6, s.upper
    # 2. all draws -> LLR ~ 0, no verdict
    for _ in range(300):
        s.update("draw")
    assert abs(s.llr) < 0.05, s.llr
    assert s.verdict() is None
    e, _ = s.elo()
    assert abs(e) < 1e-9, e
    # 3. dominant engine (~+100 elo pace) -> must hit H1
    s = TrinomialSprt(0.0, 5.0, 0.05, 0.05)
    rng = random.Random(7)
    v = None
    for i in range(3000):
        r = rng.random()
        # score ~0.64 -> ~+100 elo, draw-heavy book-like distribution
        s.update("win" if r < 0.38 else ("draw" if r < 0.90 else "loss"))
        v = s.verdict()
        if v is not None:
            break
    assert v == "H1", (v, s.games, s.llr)
    e, _ = s.elo()
    assert 50.0 < e < 250.0, e
    # 4. regressed engine (~-70 elo) -> must hit H0 quickly
    # (a truly equal engine vs elo0=0/elo1=5 drifts to H0 only very slowly,
    #  ~tens of thousands of games -- that slowness is correct SPRT behavior)
    s = TrinomialSprt(0.0, 5.0, 0.05, 0.05)
    rng = random.Random(11)
    v = None
    for i in range(6000):
        r = rng.random()
        s.update("win" if r < 0.10 else ("draw" if r < 0.70 else "loss"))
        v = s.verdict()
        if v is not None:
            break
    assert v == "H0", (v, s.games, s.llr)
    assert s.games < 3000, s.games
    # 5. elo edge cases
    s = TrinomialSprt()
    assert s.elo() == (0.0, 0.0)
    print("selftest: ALL OK "
          f"(H1 hit, H0 hit, draws neutral, bounds LLR=+-2.944)")
    return 0


if __name__ == "__main__":
    sys.exit(main())