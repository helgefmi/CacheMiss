#include "cachemiss.hpp"
#include "board.hpp"
#include "precalc.hpp"
#include "magic_tables.hpp"
#include "move.hpp"
#include "zobrist.hpp"

inline Bitboard get_rook_attacks(int square, Bitboard occupancy) {
    occupancy &= ROOK_MASKS[square];
    int index = (occupancy * ROOK_MAGICS[square]) >> ROOK_SHIFTS[square];
    return ROOK_ATTACKS[ROOK_OFFSETS[square] + index];
}

inline Bitboard get_bishop_attacks(int square, Bitboard occupancy) {
    occupancy &= BISHOP_MASKS[square];
    int index = (occupancy * BISHOP_MAGICS[square]) >> BISHOP_SHIFTS[square];
    return BISHOP_ATTACKS[BISHOP_OFFSETS[square] + index];
}

inline Bitboard get_queen_attacks(int square, Bitboard occupancy) {
    return get_rook_attacks(square, occupancy) | get_bishop_attacks(square, occupancy);
}

// Check if a square is attacked by the given color (super-piece approach)
template <Color attacker>
inline bool is_attacked(int square, const Board& board) {
    // Non-sliding pieces
    if (KNIGHT_MOVES[square] & board.pieces[(int)attacker][(int)Piece::Knight])
        return true;

    if (KING_MOVES[square] & board.pieces[(int)attacker][(int)Piece::King])
        return true;

    // Pawns - use opposite color's attack pattern
    constexpr Color defender = (attacker == Color::White) ? Color::Black : Color::White;
    if (PAWN_ATTACKS[(int)defender][square] & board.pieces[(int)attacker][(int)Piece::Pawn])
        return true;

    // Sliding pieces
    Bitboard occ = board.all_occupied;

    if (get_rook_attacks(square, occ) &
        (board.pieces[(int)attacker][(int)Piece::Rook] |
         board.pieces[(int)attacker][(int)Piece::Queen]))
        return true;

    if (get_bishop_attacks(square, occ) &
        (board.pieces[(int)attacker][(int)Piece::Bishop] |
         board.pieces[(int)attacker][(int)Piece::Queen]))
        return true;

    return false;
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

template <Color turn>
MoveList generate_moves(const Board& board) {
    MoveList moves;

    const Bitboard not_occupied = ~board.all_occupied;
    const Bitboard enemy_occupied = board.occupied[(int)turn ^ 1];

    // Separate promoting pawns from normal pawns to avoid branches in inner loops
    constexpr Bitboard RANK_7 = 0x00FF000000000000ULL;
    constexpr Bitboard RANK_2 = 0x000000000000FF00ULL;
    constexpr Bitboard PROMOTING_RANK = (turn == Color::White) ? RANK_7 : RANK_2;

    Bitboard pawns_bb = board.pieces[(int)turn][(int)Piece::Pawn];
    Bitboard promoting_pawns = pawns_bb & PROMOTING_RANK;
    Bitboard normal_pawns = pawns_bb & ~PROMOTING_RANK;

    // Promoting pawns (no double moves, no en passant possible)
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

    // Compute ep bitboard once for all pawns
    constexpr int EP_RANK = (turn == Color::White) ? 5 : 2;  // rank 6 or 3 (0-indexed)
    Bitboard ep_bb = (board.ep_file < 8) ? square_bb(EP_RANK * 8 + board.ep_file) : 0;

    // Normal pawns (no promotions possible)
    for (Bitboard bb = normal_pawns; bb; bb &= bb - 1) {
        int from_sq = lsb_index(bb);

        // Single push
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

        // Captures (including en passant)
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

    Bitboard knights_bb = board.pieces[(int)turn][(int)Piece::Knight];
    for (Bitboard from_bb = knights_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard knight_moves = KNIGHT_MOVES[from_sq] & (~board.occupied[(int)turn]);
        for (Bitboard to_bb = knight_moves; to_bb; to_bb &= to_bb - 1) {
            int to_sq = lsb_index(to_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.add(Move32(from_sq, to_sq, Piece::None, captured_piece));
        }
    }

    Bitboard rooks_bb = board.pieces[(int)turn][(int)Piece::Rook];
    for (Bitboard from_bb = rooks_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard rook_moves = get_rook_attacks(from_sq, board.all_occupied) & (~board.occupied[(int)turn]);
        for (Bitboard to_bb = rook_moves; to_bb; to_bb &= to_bb - 1) {
            int to_sq = lsb_index(to_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.add(Move32(from_sq, to_sq, Piece::None, captured_piece));
        }
    }

    Bitboard bishops_bb = board.pieces[(int)turn][(int)Piece::Bishop];
    for (Bitboard from_bb = bishops_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard bishop_moves = get_bishop_attacks(from_sq, board.all_occupied) & (~board.occupied[(int)turn]);
        for (Bitboard to_bb = bishop_moves; to_bb; to_bb &= to_bb - 1) {
            int to_sq = lsb_index(to_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.add(Move32(from_sq, to_sq, Piece::None, captured_piece));
        }
    }

    Bitboard queens_bb = board.pieces[(int)turn][(int)Piece::Queen];
    for (Bitboard from_bb = queens_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard queen_moves = get_queen_attacks(from_sq, board.all_occupied) & (~board.occupied[(int)turn]);
        for (Bitboard to_bb = queen_moves; to_bb; to_bb &= to_bb - 1) {
            int to_sq = lsb_index(to_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.add(Move32(from_sq, to_sq, Piece::None, captured_piece));
        }
    }

    {
        Bitboard king_bb = board.pieces[(int)turn][(int)Piece::King];
        int from_sq = lsb_index(king_bb);
        Bitboard king_moves = KING_MOVES[from_sq] & (~board.occupied[(int)turn]);
        for (Bitboard to_bb = king_moves; to_bb; to_bb &= to_bb - 1) {
            int to_sq = lsb_index(to_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.add(Move32(from_sq, to_sq, Piece::None, captured_piece));
        }

        // Castling
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

    return moves;
}

MoveList generate_moves(const Board& board) {
    if (board.turn == Color::White) {
        return generate_moves<Color::White>(board);
    } else {
        return generate_moves<Color::Black>(board);
    }
}

// Castling rook squares
constexpr int A1 = 0, H1 = 7, A8 = 56, H8 = 63;

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

    // Handle captures
    if (captured != Piece::None) {
        if (move.is_en_passant()) {
            int captured_sq = (turn == Color::White) ? to - 8 : to + 8;
            board.pieces_on_square[captured_sq] = Piece::None;
            board.occupied[(int)enemy] &= ~square_bb(captured_sq);
            board.pieces[(int)enemy][(int)Piece::Pawn] &= ~square_bb(captured_sq);
            h ^= zobrist::pieces[(int)enemy][(int)Piece::Pawn][captured_sq];
        } else {
            board.occupied[(int)enemy] &= ~square_bb(to);
            board.pieces[(int)enemy][(int)captured] &= ~square_bb(to);
            h ^= zobrist::pieces[(int)enemy][(int)captured][to];
        }
    }

    // Handle castling
    if (move.is_castling()) {
        int rook_from, rook_to;
        if (to == G1) { rook_from = H1; rook_to = F1; }
        else if (to == C1) { rook_from = A1; rook_to = D1; }
        else if (to == G8) { rook_from = H8; rook_to = F8; }
        else { rook_from = A8; rook_to = D8; }

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

    u64 h = board.hash ^ zobrist::side_to_move;

    // Restore en passant
    if (board.ep_file < 8) {
        h ^= zobrist::ep_file[board.ep_file];
    }
    board.ep_file = move.prev_ep_file();
    if (board.ep_file < 8) {
        h ^= zobrist::ep_file[board.ep_file];
    }

    // Restore castling rights
    h ^= zobrist::castling[board.castling];
    board.castling = move.prev_castling();
    h ^= zobrist::castling[board.castling];

    // Move piece back
    board.pieces_on_square[from] = piece;
    board.pieces_on_square[to] = Piece::None;
    board.occupied[(int)turn] |= square_bb(from);
    board.occupied[(int)turn] &= ~square_bb(to);
    board.pieces[(int)turn][(int)piece] |= square_bb(from);
    board.pieces[(int)turn][(int)to_piece] &= ~square_bb(to);
    h ^= zobrist::pieces[(int)turn][(int)piece][from];
    h ^= zobrist::pieces[(int)turn][(int)to_piece][to];

    // Restore captured piece
    if (captured != Piece::None) {
        if (move.is_en_passant()) {
            int captured_sq = (turn == Color::White) ? to - 8 : to + 8;
            board.pieces_on_square[captured_sq] = Piece::Pawn;
            board.occupied[(int)enemy] |= square_bb(captured_sq);
            board.pieces[(int)enemy][(int)Piece::Pawn] |= square_bb(captured_sq);
            h ^= zobrist::pieces[(int)enemy][(int)Piece::Pawn][captured_sq];
        } else {
            board.pieces_on_square[to] = captured;
            board.occupied[(int)enemy] |= square_bb(to);
            board.pieces[(int)enemy][(int)captured] |= square_bb(to);
            h ^= zobrist::pieces[(int)enemy][(int)captured][to];
        }
    }

    // Undo castling rook move
    if (move.is_castling()) {
        int rook_from, rook_to;
        if (to == G1) { rook_from = H1; rook_to = F1; }
        else if (to == C1) { rook_from = A1; rook_to = D1; }
        else if (to == G8) { rook_from = H8; rook_to = F8; }
        else { rook_from = A8; rook_to = D8; }

        board.pieces_on_square[rook_from] = Piece::Rook;
        board.pieces_on_square[rook_to] = Piece::None;
        board.occupied[(int)turn] |= square_bb(rook_from);
        board.occupied[(int)turn] &= ~square_bb(rook_to);
        board.pieces[(int)turn][(int)Piece::Rook] |= square_bb(rook_from);
        board.pieces[(int)turn][(int)Piece::Rook] &= ~square_bb(rook_to);
        h ^= zobrist::pieces[(int)turn][(int)Piece::Rook][rook_from];
        h ^= zobrist::pieces[(int)turn][(int)Piece::Rook][rook_to];
    }

    // Update all_occupied
    board.all_occupied = board.occupied[0] | board.occupied[1];
    board.hash = h;
}

bool is_attacked(int square, Color attacker, const Board& board) {
    if (attacker == Color::White) {
        return is_attacked<Color::White>(square, board);
    } else {
        return is_attacked<Color::Black>(square, board);
    }
}
