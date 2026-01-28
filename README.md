# CacheMiss

A UCI chess engine written in C++20.

Watch CacheMissBot play at https://lichess.org/@/CacheMissBot from time to time.

## Features

### Board Representation
- Bitboard representation with magic bitboards for sliding pieces
- Incremental Zobrist hashing for position and pawn structure
- Incremental game phase tracking for tapered evaluation

### Search
- Alpha-beta with iterative deepening and principal variation search (PVS)
- Transposition table with 16-byte entries, age-aware replacement, and TT prefetching
- Aspiration windows with dynamic widening
- Late move reductions (LMR) with log-based reduction table
- Null-move pruning (NMP) with verification
- Check extensions
- Static exchange evaluation (SEE) with threshold optimization for pruning
- Move ordering: TT move → MVV-LVA (good captures) → killers → history heuristic → bad captures
- Repetition detection and 50-move rule

### Evaluation
- Tapered evaluation interpolating between middlegame and endgame scores
- Piece-square tables (PST)
- Piece mobility (safe squares not attacked by enemy pawns)
- Rook x-ray mobility through friendly rooks
- Bishop pair bonus
- Rook on open/semi-open files
- Rook on 7th rank
- Pawn structure: doubled, isolated, backward pawn penalties
- Passed pawns with rank-based bonuses, protected/connected passer bonuses
- Space control (center and extended center)
- King safety (attacks on enemy king zone)
- Pawn structure cache (1 MB) for efficient reuse

### UCI Protocol
- Full UCI support with pondering
- Configurable hash size and move overhead

## Building

### Requirements
- CMake 3.20+
- C++20 compiler (clang++ or g++)

### Build

**Development (with debug symbols for profiling):**
```bash
cmake -S . -B build
cmake --build build
```

**Release (maximum performance):**
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Command Line Options

```
./build/cachemiss [options]

Options:
  --fen <fen>                            Set position (default: starting position)
  --perft <depth>                        Run perft to given depth
  --divide <depth>                       Run divide (perft per move) to given depth
  --search[=time]                        Search for best move (time in ms, default: 10000)
  --bench-perftsuite <file>[=max_depth]  Run perft test suite
  --bench-wac <file>[=time_ms]           Run WAC test suite (default: 1000ms)
  --wac-id <id>                          Filter WAC suite to single position
  --mem <mb>                             Hash table size in MB (default: 512)
  -h, --help                             Show this help
```

Running without options starts UCI mode.

## Testing

```bash
# Run the full test suite
./build/run_tests

# Run specific test category
./build/run_tests MoveGen   # Move generation tests
./build/run_tests SEE       # Static exchange evaluation tests
./build/run_tests Eval      # Evaluation tests
./build/run_tests Search    # Search tests
./build/run_tests Perft     # Perft correctness tests
```

## UCI Options

| Option | Default | Description |
|--------|---------|-------------|
| Hash | 512 | Transposition table size in MB |
| Move Overhead | 100 | Time buffer for network lag (ms) |
| Ponder | false | Think on opponent's time |

## Tools

- `match` - TUI match supervisor for engine vs engine games (uses FTXUI)
- `pgn2epd` - Convert PGN files to EPD format
- `tune_eval` - Tune all evaluation parameters (~940) from PGN data using gradient descent
- `gen_magics` - Generate magic bitboard tables for sliding pieces
- `wac_compare` - Compare WAC test results between engine versions
- `run_tests` - Test suite for move generation, SEE, evaluation, search, and UCI parsing

## Lichess Bot

See [lichess-bot/README.md](lichess-bot/README.md) for running CacheMiss on Lichess.

```bash
git submodule update --init
cp config.yml.example config.yml
# Edit config.yml with your Lichess API token
./scripts/lichess-bot.sh
```

## License

MIT - see [LICENSE](LICENSE)
