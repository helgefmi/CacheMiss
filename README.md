# CacheMiss

A UCI chess engine written in C++20.

## Features

- Bitboard representation with magic bitboards for sliding pieces
- Alpha-beta search with iterative deepening
- Transposition table with age-aware replacement
- Late move reductions (LMR) and null-move pruning (NMP)
- Aspiration windows and check extensions
- Tapered evaluation with piece-square tables
- Pawn structure evaluation (doubled, isolated, passed pawns)
- UCI protocol with pondering support

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
  --tests                                Run test suite
  --mem <mb>                             Hash table size in MB (default: 512)
  -h, --help                             Show this help
```

Running without options starts UCI mode.

## UCI Options

| Option | Default | Description |
|--------|---------|-------------|
| Hash | 512 | Transposition table size in MB |
| Move Overhead | 100 | Time buffer for network lag (ms) |
| Ponder | false | Think on opponent's time |

## Tools

- `match` - TUI match supervisor for engine vs engine games
- `pgn2epd` - Convert PGN files to EPD format
- `tune_pst` - Tune piece-square tables from PGN data

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
