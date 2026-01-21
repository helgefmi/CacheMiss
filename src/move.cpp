#include "cachemiss.hpp"
#include "board.hpp"
#include "precalc.hpp"
#include "move.hpp"
#include "zobrist.hpp"
#include <cassert>

// Check if a square is attacked by the given color (super-piece approach)
template <Color attacker>
inline bool is_attacked(int square, const Board& board) {
    constexpr Color defender = (attacker == Color::White) ? Color::Black : Color::White;
    Bitboard occ = board.all_occupied;
    return (
        (KNIGHT_MOVES[square] & board.pieces[(int)attacker][(int)Piece::Knight]) |
        (KING_MOVES[square] & board.pieces[(int)attacker][(int)Piece::King]) |
        (PAWN_ATTACKS[(int)defender][square] & board.pieces[(int)attacker][(int)Piece::Pawn]) |
        (get_rook_attacks(square, occ) & (board.pieces[(int)attacker][(int)Piece::Rook] | board.pieces[(int)attacker][(int)Piece::Queen])) |
        (get_bishop_attacks(square, occ) & (board.pieces[(int)attacker][(int)Piece::Bishop] | board.pieces[(int)attacker][(int)Piece::Queen]))
    );
}

// Castling constants
constexpr int E1 = 4, G1 = 6, C1 = 2, F1 = 5, D1 = 3;
constexpr int E8 = 60, G8 = 62, C8 = 58, F8 = 61, D8 = 59;

// Castling rights: bit0=wQ, bit1=wK, bit2=bQ, bit3=bK
constexpr u8 WHITE_OO_RIGHT  = 2;   // bit 1 (kingside)
constexpr u8 WHITE_OOO_RIGHT = 1;   // bit 0 (queenside)
constexpr u8 BLACK_OO_RIGHT  = 8;   // bit 3 (kingside)
constexpr u8 BLACK_OOO_RIGHT = 4;   // bit 2 (queenside)

constexpr Bitboard WHITE_OO_PATH  = (1ULL << F1) | (1ULL << G1);
constexpr Bitboard WHITE_OOO_PATH = (1ULL << 1) | (1ULL << C1) | (1ULL << D1);  // b1, c1, d1
constexpr Bitboard BLACK_OO_PATH  = (1ULL << F8) | (1ULL << G8);
constexpr Bitboard BLACK_OOO_PATH = (1ULL << 57) | (1ULL << C8) | (1ULL << D8); // b8, c8, d8

template <Color turn, MoveType type>
MoveList generate_moves(const Board& board) {
    MoveList moves;

    constexpr bool gen_noisy = (type == MoveType::All || type == MoveType::Noisy);
    constexpr bool gen_quiet = (type == MoveType::All || type == MoveType::Quiet);

    const Bitboard not_occupied = ~board.all_occupied;
    const Bitboard enemy_occupied = board.occupied[(int)turn ^ 1];

    // Separate promoting pawns from normal pawns to avoid branches in inner loops
    constexpr Bitboard RANK_7 = 0x00FF000000000000ULL;
    constexpr Bitboard RANK_2 = 0x000000000000FF00ULL;
    constexpr Bitboard PROMOTING_RANK = (turn == Color::White) ? RANK_7 : RANK_2;

    Bitboard pawns_bb = board.pieces[(int)turn][(int)Piece::Pawn];
    Bitboard promoting_pawns = pawns_bb & PROMOTING_RANK;
    Bitboard normal_pawns = pawns_bb & ~PROMOTING_RANK;

    // Promoting pawns - all promotions are noisy (tactical moves)
    if constexpr (gen_noisy) {
        for (Bitboard bb = promoting_pawns; bb; bb &= bb - 1) {
            int from_sq = lsb_index(bb);

            // Push promotions
            Bitboard single_move = PAWN_MOVES_ONE[(int)turn][from_sq] & not_occupied;
            if (single_move) {
                int to_sq = lsb_index(single_move);
                moves.add(Move32(from_sq, to_sq, Piece::Queen));
                moves.add(Move32(from_sq, to_sq, Piece::Rook));
                moves.add(Move32(from_sq, to_sq, Piece::Bishop));
                moves.add(Move32(from_sq, to_sq, Piece::Knight));
            }

            // Capture promotions
            Bitboard captures = PAWN_ATTACKS[(int)turn][from_sq] & enemy_occupied;
            for (Bitboard cap_bb = captures; cap_bb; cap_bb &= cap_bb - 1) {
                int to_sq = lsb_index(cap_bb);
                Piece captured_piece = board.pieces_on_square[to_sq];
                moves.add(Move32(from_sq, to_sq, Piece::Queen, captured_piece));
                moves.add(Move32(from_sq, to_sq, Piece::Rook, captured_piece));
                moves.add(Move32(from_sq, to_sq, Piece::Bishop, captured_piece));
                moves.add(Move32(from_sq, to_sq, Piece::Knight, captured_piece));
            }
        }
    }

    // Compute ep bitboard once for all pawns
    constexpr int EP_RANK = (turn == Color::White) ? 5 : 2;  // rank 6 or 3 (0-indexed)
    Bitboard ep_bb = (board.ep_file < 8) ? square_bb(EP_RANK * 8 + board.ep_file) : 0;

    // Normal pawns (no promotions possible)
    for (Bitboard bb = normal_pawns; bb; bb &= bb - 1) {
        int from_sq = lsb_index(bb);

        // Single and double pushes are quiet
        if constexpr (gen_quiet) {
            Bitboard single_move = PAWN_MOVES_ONE[(int)turn][from_sq] & not_occupied;
            if (single_move) {
                int to_sq = lsb_index(single_move);
                moves.add(Move32(from_sq, to_sq));

                // Double push
                Bitboard double_move = PAWN_MOVES_TWO[(int)turn][from_sq] & not_occupied;
                if (double_move) {
                    int to_sq_double = lsb_index(double_move);
                    moves.add(Move32(from_sq, to_sq_double));
                }
            }
        }

        // Captures (including en passant) are noisy
        if constexpr (gen_noisy) {
            Bitboard captures = PAWN_ATTACKS[(int)turn][from_sq] & (enemy_occupied | ep_bb);
            for (Bitboard cap_bb = captures; cap_bb; cap_bb &= cap_bb - 1) {
                int to_sq = lsb_index(cap_bb);
                bool is_ep = square_bb(to_sq) & ep_bb;
                Piece captured_piece = is_ep ? Piece::Pawn : board.pieces_on_square[to_sq];
                Move32 m(from_sq, to_sq, Piece::None, captured_piece);
                if (is_ep) m.set_en_passant();
                moves.add(m);
            }
        }
    }

    // Knights - captures are noisy, non-captures are quiet
    Bitboard knights_bb = board.pieces[(int)turn][(int)Piece::Knight];
    for (Bitboard from_bb = knights_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard targets = KNIGHT_MOVES[from_sq] & (~board.occupied[(int)turn]);
        if constexpr (gen_noisy) {
            for (Bitboard to_bb = targets & enemy_occupied; to_bb; to_bb &= to_bb - 1) {
                int to_sq = lsb_index(to_bb);
                moves.add(Move32(from_sq, to_sq, Piece::None, board.pieces_on_square[to_sq]));
            }
        }
        if constexpr (gen_quiet) {
            for (Bitboard to_bb = targets & not_occupied; to_bb; to_bb &= to_bb - 1) {
                int to_sq = lsb_index(to_bb);
                moves.add(Move32(from_sq, to_sq));
            }
        }
    }

    // Rooks
    Bitboard rooks_bb = board.pieces[(int)turn][(int)Piece::Rook];
    for (Bitboard from_bb = rooks_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard targets = get_rook_attacks(from_sq, board.all_occupied) & (~board.occupied[(int)turn]);
        if constexpr (gen_noisy) {
            for (Bitboard to_bb = targets & enemy_occupied; to_bb; to_bb &= to_bb - 1) {
                int to_sq = lsb_index(to_bb);
                moves.add(Move32(from_sq, to_sq, Piece::None, board.pieces_on_square[to_sq]));
            }
        }
        if constexpr (gen_quiet) {
            for (Bitboard to_bb = targets & not_occupied; to_bb; to_bb &= to_bb - 1) {
                int to_sq = lsb_index(to_bb);
                moves.add(Move32(from_sq, to_sq));
            }
        }
    }

    // Bishops
    Bitboard bishops_bb = board.pieces[(int)turn][(int)Piece::Bishop];
    for (Bitboard from_bb = bishops_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard targets = get_bishop_attacks(from_sq, board.all_occupied) & (~board.occupied[(int)turn]);
        if constexpr (gen_noisy) {
            for (Bitboard to_bb = targets & enemy_occupied; to_bb; to_bb &= to_bb - 1) {
                int to_sq = lsb_index(to_bb);
                moves.add(Move32(from_sq, to_sq, Piece::None, board.pieces_on_square[to_sq]));
            }
        }
        if constexpr (gen_quiet) {
            for (Bitboard to_bb = targets & not_occupied; to_bb; to_bb &= to_bb - 1) {
                int to_sq = lsb_index(to_bb);
                moves.add(Move32(from_sq, to_sq));
            }
        }
    }

    // Queens
    Bitboard queens_bb = board.pieces[(int)turn][(int)Piece::Queen];
    for (Bitboard from_bb = queens_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard targets = get_queen_attacks(from_sq, board.all_occupied) & (~board.occupied[(int)turn]);
        if constexpr (gen_noisy) {
            for (Bitboard to_bb = targets & enemy_occupied; to_bb; to_bb &= to_bb - 1) {
                int to_sq = lsb_index(to_bb);
                moves.add(Move32(from_sq, to_sq, Piece::None, board.pieces_on_square[to_sq]));
            }
        }
        if constexpr (gen_quiet) {
            for (Bitboard to_bb = targets & not_occupied; to_bb; to_bb &= to_bb - 1) {
                int to_sq = lsb_index(to_bb);
                moves.add(Move32(from_sq, to_sq));
            }
        }
    }

    // King
    {
        Bitboard king_bb = board.pieces[(int)turn][(int)Piece::King];
        int from_sq = lsb_index(king_bb);
        Bitboard targets = KING_MOVES[from_sq] & (~board.occupied[(int)turn]);

        if constexpr (gen_noisy) {
            for (Bitboard to_bb = targets & enemy_occupied; to_bb; to_bb &= to_bb - 1) {
                int to_sq = lsb_index(to_bb);
                moves.add(Move32(from_sq, to_sq, Piece::None, board.pieces_on_square[to_sq]));
            }
        }
        if constexpr (gen_quiet) {
            for (Bitboard to_bb = targets & not_occupied; to_bb; to_bb &= to_bb - 1) {
                int to_sq = lsb_index(to_bb);
                moves.add(Move32(from_sq, to_sq));
            }
        }

        // Castling is quiet
        if constexpr (gen_quiet) {
            constexpr Color enemy = (turn == Color::White) ? Color::Black : Color::White;

            // Castling: check rights, path empty, king not in check, pass-through not attacked
            // Destination check removed - handled by is_illegal() after make_move
            if constexpr (turn == Color::White) {
                if (from_sq == E1) {
                    // Kingside: rights + path empty + e1,f1 not attacked
                    if ((board.castling & WHITE_OO_RIGHT) &&
                        !(board.all_occupied & WHITE_OO_PATH) &&
                        !is_attacked<enemy>(E1, board) &&
                        !is_attacked<enemy>(F1, board)) {
                        Move32 m(E1, G1);
                        m.set_castling();
                        moves.add(m);
                    }
                    // Queenside: rights + path empty + e1,d1 not attacked
                    if ((board.castling & WHITE_OOO_RIGHT) &&
                        !(board.all_occupied & WHITE_OOO_PATH) &&
                        !is_attacked<enemy>(E1, board) &&
                        !is_attacked<enemy>(D1, board)) {
                        Move32 m(E1, C1);
                        m.set_castling();
                        moves.add(m);
                    }
                }
            } else {
                if (from_sq == E8) {
                    // Kingside: rights + path empty + e8,f8 not attacked
                    if ((board.castling & BLACK_OO_RIGHT) &&
                        !(board.all_occupied & BLACK_OO_PATH) &&
                        !is_attacked<enemy>(E8, board) &&
                        !is_attacked<enemy>(F8, board)) {
                        Move32 m(E8, G8);
                        m.set_castling();
                        moves.add(m);
                    }
                    // Queenside: rights + path empty + e8,d8 not attacked
                    if ((board.castling & BLACK_OOO_RIGHT) &&
                        !(board.all_occupied & BLACK_OOO_PATH) &&
                        !is_attacked<enemy>(E8, board) &&
                        !is_attacked<enemy>(D8, board)) {
                        Move32 m(E8, C8);
                        m.set_castling();
                        moves.add(m);
                    }
                }
            }
        }
    }

    return moves;
}

// Explicit template instantiations
template MoveList generate_moves<Color::White, MoveType::All>(const Board&);
template MoveList generate_moves<Color::White, MoveType::Noisy>(const Board&);
template MoveList generate_moves<Color::White, MoveType::Quiet>(const Board&);
template MoveList generate_moves<Color::Black, MoveType::All>(const Board&);
template MoveList generate_moves<Color::Black, MoveType::Noisy>(const Board&);
template MoveList generate_moves<Color::Black, MoveType::Quiet>(const Board&);

template <MoveType type>
MoveList generate_moves(const Board& board) {
    if (board.turn == Color::White) {
        return generate_moves<Color::White, type>(board);
    } else {
        return generate_moves<Color::Black, type>(board);
    }
}

// Explicit instantiations for MoveType-only wrapper
template MoveList generate_moves<MoveType::All>(const Board&);
template MoveList generate_moves<MoveType::Noisy>(const Board&);
template MoveList generate_moves<MoveType::Quiet>(const Board&);

// Castling rook squares
constexpr int A1 = 0, H1 = 7, A8 = 56, H8 = 63;

// Map king destination to rook from/to squares for castling
inline std::pair<int, int> get_castling_rook_squares(int king_to) {
    switch (king_to) {
        case G1: return {H1, F1};
        case C1: return {A1, D1};
        case G8: return {H8, F8};
        default: return {A8, D8};  // C8
    }
}

// Castling rights masks: AND with these to clear rights when piece moves from/to square
// bit0=wQ, bit1=wK, bit2=bQ, bit3=bK
constexpr u8 CASTLING_MASK[64] = {
    0xE, 0xF, 0xF, 0xF, 0xC, 0xF, 0xF, 0xD,  // rank 1: a1=~wQ, e1=~(wQ|wK), h1=~wK
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
    0xB, 0xF, 0xF, 0xF, 0x3, 0xF, 0xF, 0x7   // rank 8: a8=~bQ, e8=~(bQ|bK), h8=~bK
};

void make_move(Board& board, Move32& move) {
    const int from = move.from();
    const int to = move.to();
    const Piece promotion = move.promotion();
    const Piece captured = move.captured();
    const Color turn = board.turn;
    const Color enemy = opposite(turn);
    const Piece piece = board.pieces_on_square[from];
    const Piece to_piece = promotion != Piece::None ? promotion : piece;

    // Save undo info
    move.set_undo_info(board.castling, board.ep_file);
    assert(board.undo_sp < 1024 && "Undo stack overflow");
    board.undo_stack[board.undo_sp] = {board.hash, board.pawn_key, board.halfmove_clock};
    board.undo_sp++;

    // Update halfmove clock (reset on pawn move or capture, otherwise increment)
    if (piece == Piece::Pawn || captured != Piece::None) {
        board.halfmove_clock = 0;
    } else {
        board.halfmove_clock++;
    }

    // Flip turn
    u64 h = board.hash ^ zobrist::side_to_move;
    board.turn = enemy;

    // Clear en passant
    if (board.ep_file < 8) {
        h ^= zobrist::ep_file[board.ep_file];
    }
    board.ep_file = 8;

    // Move the piece
    board.pieces_on_square[to] = to_piece;
    board.pieces_on_square[from] = Piece::None;
    board.occupied[(int)turn] &= ~square_bb(from);
    board.occupied[(int)turn] |= square_bb(to);
    board.pieces[(int)turn][(int)piece] &= ~square_bb(from);
    board.pieces[(int)turn][(int)to_piece] |= square_bb(to);
    h ^= zobrist::pieces[(int)turn][(int)piece][from];
    h ^= zobrist::pieces[(int)turn][(int)to_piece][to];
    if (piece == Piece::King) {
        board.king_sq[(int)turn] = to;
    }

    // Update pawn_key for pawn movements
    if (piece == Piece::Pawn) {
        board.pawn_key ^= zobrist::pieces[(int)turn][(int)Piece::Pawn][from];
        if (promotion == Piece::None) {
            board.pawn_key ^= zobrist::pieces[(int)turn][(int)Piece::Pawn][to];
        }
        // Note: promotions remove the pawn entirely, so no XOR in for to square
    }

    // Handle captures
    if (captured != Piece::None) {
        // Update phase for captured piece
        board.phase -= PHASE_VALUES[(int)captured];

        if (move.is_en_passant()) {
            int captured_sq = (turn == Color::White) ? to - 8 : to + 8;
            board.pieces_on_square[captured_sq] = Piece::None;
            board.occupied[(int)enemy] &= ~square_bb(captured_sq);
            board.pieces[(int)enemy][(int)Piece::Pawn] &= ~square_bb(captured_sq);
            h ^= zobrist::pieces[(int)enemy][(int)Piece::Pawn][captured_sq];
            board.pawn_key ^= zobrist::pieces[(int)enemy][(int)Piece::Pawn][captured_sq];
        } else {
            board.occupied[(int)enemy] &= ~square_bb(to);
            board.pieces[(int)enemy][(int)captured] &= ~square_bb(to);
            h ^= zobrist::pieces[(int)enemy][(int)captured][to];
            if (captured == Piece::Pawn) {
                board.pawn_key ^= zobrist::pieces[(int)enemy][(int)Piece::Pawn][to];
            }
        }
    }

    // Update phase for promotion (pawn becomes higher-value piece)
    if (promotion != Piece::None) {
        board.phase += PHASE_VALUES[(int)promotion];
    }

    // Handle castling
    if (move.is_castling()) {
        auto [rook_from, rook_to] = get_castling_rook_squares(to);

        board.pieces_on_square[rook_to] = Piece::Rook;
        board.pieces_on_square[rook_from] = Piece::None;
        board.occupied[(int)turn] &= ~square_bb(rook_from);
        board.occupied[(int)turn] |= square_bb(rook_to);
        board.pieces[(int)turn][(int)Piece::Rook] &= ~square_bb(rook_from);
        board.pieces[(int)turn][(int)Piece::Rook] |= square_bb(rook_to);
        h ^= zobrist::pieces[(int)turn][(int)Piece::Rook][rook_from];
        h ^= zobrist::pieces[(int)turn][(int)Piece::Rook][rook_to];
    }

    // Update all_occupied
    board.all_occupied = board.occupied[0] | board.occupied[1];

    // Update castling rights
    h ^= zobrist::castling[board.castling];
    board.castling &= CASTLING_MASK[from] & CASTLING_MASK[to];
    h ^= zobrist::castling[board.castling];

    // Set en passant target if double pawn push
    if (piece == Piece::Pawn) {
        int diff = to - from;
        if (diff == 16 || diff == -16) {
            board.ep_file = from % 8;
            h ^= zobrist::ep_file[board.ep_file];
        }
    }

    board.hash = h;
}

void unmake_move(Board& board, const Move32& move) {
    const int from = move.from();
    const int to = move.to();
    const Piece promotion = move.promotion();
    const Piece captured = move.captured();

    // Flip turn back
    board.turn = opposite(board.turn);
    const Color turn = board.turn;
    const Color enemy = opposite(turn);

    // Determine original piece (pawn if this was a promotion)
    const Piece to_piece = board.pieces_on_square[to];
    const Piece piece = (promotion != Piece::None) ? Piece::Pawn : to_piece;

    // Restore en passant and castling rights
    board.ep_file = move.prev_ep_file();
    board.castling = move.prev_castling();

    // Move piece back
    board.pieces_on_square[from] = piece;
    board.pieces_on_square[to] = Piece::None;
    board.occupied[(int)turn] |= square_bb(from);
    board.occupied[(int)turn] &= ~square_bb(to);
    board.pieces[(int)turn][(int)piece] |= square_bb(from);
    board.pieces[(int)turn][(int)to_piece] &= ~square_bb(to);
    if (piece == Piece::King) {
        board.king_sq[(int)turn] = from;
    }

    // Restore captured piece
    if (captured != Piece::None) {
        // Restore phase for captured piece
        board.phase += PHASE_VALUES[(int)captured];

        if (move.is_en_passant()) {
            int captured_sq = (turn == Color::White) ? to - 8 : to + 8;
            board.pieces_on_square[captured_sq] = Piece::Pawn;
            board.occupied[(int)enemy] |= square_bb(captured_sq);
            board.pieces[(int)enemy][(int)Piece::Pawn] |= square_bb(captured_sq);
        } else {
            board.pieces_on_square[to] = captured;
            board.occupied[(int)enemy] |= square_bb(to);
            board.pieces[(int)enemy][(int)captured] |= square_bb(to);
        }
    }

    // Restore phase for undone promotion
    if (promotion != Piece::None) {
        board.phase -= PHASE_VALUES[(int)promotion];
    }

    // Undo castling rook move
    if (move.is_castling()) {
        auto [rook_from, rook_to] = get_castling_rook_squares(to);

        board.pieces_on_square[rook_from] = Piece::Rook;
        board.pieces_on_square[rook_to] = Piece::None;
        board.occupied[(int)turn] |= square_bb(rook_from);
        board.occupied[(int)turn] &= ~square_bb(rook_to);
        board.pieces[(int)turn][(int)Piece::Rook] |= square_bb(rook_from);
        board.pieces[(int)turn][(int)Piece::Rook] &= ~square_bb(rook_to);
    }

    // Update all_occupied and restore hash/halfmove/pawn_key from stack
    board.all_occupied = board.occupied[0] | board.occupied[1];
    --board.undo_sp;
    const UndoInfo& undo = board.undo_stack[board.undo_sp];
    board.hash = undo.hash;
    board.pawn_key = undo.pawn_key;
    board.halfmove_clock = undo.halfmove_clock;
}

void make_null_move(Board& b, int& prev_ep_file) {
    // Save en passant file for unmake
    prev_ep_file = b.ep_file;

    // Update hash
    b.hash ^= zobrist::side_to_move;
    if (b.ep_file != 8) {
        b.hash ^= zobrist::ep_file[b.ep_file];
        b.ep_file = 8;
    }

    // Flip turn
    b.turn = opposite(b.turn);

    // Push to stack (for repetition detection)
    assert(b.undo_sp < 1024 && "Undo stack overflow");
    b.undo_stack[b.undo_sp++] = {b.hash, b.pawn_key, b.halfmove_clock};
}

void unmake_null_move(Board& b, int prev_ep_file) {
    // Pop undo stack
    --b.undo_sp;

    // Restore hash
    b.hash ^= zobrist::side_to_move;
    if (prev_ep_file != 8) {
        b.hash ^= zobrist::ep_file[prev_ep_file];
    }

    // Restore en passant and turn
    b.ep_file = prev_ep_file;
    b.turn = opposite(b.turn);
}

bool is_attacked(int square, Color attacker, const Board& board) {
    if (attacker == Color::White) {
        return is_attacked<Color::White>(square, board);
    } else {
        return is_attacked<Color::Black>(square, board);
    }
}

bool is_illegal(const Board& board) {
    Color them = board.turn;  // Side to move next
    Color us = opposite(them);  // Side that just moved
    return is_attacked(board.king_sq[(int)us], them, board);
}

std::string Move32::to_uci() const {
    int from_sq = from();
    int to_sq = to();

    std::string uci;
    uci += 'a' + (from_sq % 8);
    uci += '1' + (from_sq / 8);
    uci += 'a' + (to_sq % 8);
    uci += '1' + (to_sq / 8);

    if (is_promotion()) {
        // UCI uses lowercase for promotion piece
        char promo = piece_to_char(promotion());
        uci += (promo >= 'A' && promo <= 'Z') ? (promo + 32) : promo;
    }

    return uci;
}

Move32 parse_uci_move(const std::string& uci, Board& board) {
    if (uci.length() < 4) return Move32(0);

    int from_file = uci[0] - 'a';
    int from_rank = uci[1] - '1';
    int to_file = uci[2] - 'a';
    int to_rank = uci[3] - '1';

    if (from_file < 0 || from_file > 7 || from_rank < 0 || from_rank > 7 ||
        to_file < 0 || to_file > 7 || to_rank < 0 || to_rank > 7) {
        return Move32(0);
    }

    int from_sq = from_rank * 8 + from_file;
    int to_sq = to_rank * 8 + to_file;

    Piece promo = Piece::None;
    if (uci.length() >= 5) {
        switch (uci[4]) {
            case 'q': promo = Piece::Queen; break;
            case 'r': promo = Piece::Rook; break;
            case 'b': promo = Piece::Bishop; break;
            case 'n': promo = Piece::Knight; break;
        }
    }

    // Generate legal moves and find matching one
    MoveList moves = generate_moves(board);
    for (int i = 0; i < moves.size; ++i) {
        Move32& m = moves[i];
        if (m.from() == from_sq && m.to() == to_sq) {
            // For promotions, also match the promotion piece
            if (promo != Piece::None) {
                if (m.promotion() == promo) {
                    return m;
                }
            } else if (!m.is_promotion()) {
                return m;
            }
        }
    }

    return Move32(0);
}

std::string Move32::to_string(const Board& board) const {
    int from_sq = from();
    int to_sq = to();
    Piece piece = board.pieces_on_square[from_sq];

    // Castling
    if (is_castling()) {
        int file_diff = (to_sq % 8) - (from_sq % 8);
        return file_diff > 0 ? "O-O" : "O-O-O";
    }

    std::string san;

    // Piece letter (not for pawns)
    if (piece != Piece::Pawn) {
        san += piece_to_char(piece);
    }

    // Disambiguation for non-pawn pieces
    if (piece != Piece::Pawn) {
        MoveList moves = generate_moves(board);
        bool need_file = false, need_rank = false;

        for (const auto& m : moves) {
            if (m.to() == to_sq && m.from() != from_sq &&
                board.pieces_on_square[m.from()] == piece) {
                // Another piece of same type can move to same square
                if (m.from() % 8 == from_sq % 8) need_rank = true;
                else need_file = true;
            }
        }

        if (need_file || (need_rank && (from_sq % 8) != (to_sq % 8))) {
            san += 'a' + (from_sq % 8);
        }
        if (need_rank) {
            san += '1' + (from_sq / 8);
        }
    } else if (is_capture() || is_en_passant()) {
        // Pawn captures include source file
        san += 'a' + (from_sq % 8);
    }

    // Capture indicator
    if (is_capture() || is_en_passant()) {
        san += 'x';
    }

    // Destination square
    san += 'a' + (to_sq % 8);
    san += '1' + (to_sq / 8);

    // Promotion
    if (is_promotion()) {
        san += '=';
        san += piece_to_char(promotion());
    }

    return san;
}
