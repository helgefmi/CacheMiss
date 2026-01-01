#include "cachemiss.hpp"
#include "board.hpp"
#include "precalc.hpp"
#include "magic_tables.hpp"
#include "move.hpp"

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
    Bitboard occ = board.all_occupied;

    // Sliding pieces
    if (get_rook_attacks(square, occ) &
        (board.pieces[(int)attacker][(int)Piece::Rook] |
         board.pieces[(int)attacker][(int)Piece::Queen]))
        return true;

    if (get_bishop_attacks(square, occ) &
        (board.pieces[(int)attacker][(int)Piece::Bishop] |
         board.pieces[(int)attacker][(int)Piece::Queen]))
        return true;

    // Non-sliding pieces
    if (KNIGHT_MOVES[square] & board.pieces[(int)attacker][(int)Piece::Knight])
        return true;

    if (KING_MOVES[square] & board.pieces[(int)attacker][(int)Piece::King])
        return true;

    // Pawns - use opposite color's attack pattern
    constexpr Color defender = (attacker == Color::White) ? Color::Black : Color::White;
    if (PAWN_ATTACKS[(int)defender][square] & board.pieces[(int)attacker][(int)Piece::Pawn])
        return true;

    return false;
}

// Castling constants
constexpr int E1 = 4, G1 = 6, C1 = 2, F1 = 5, D1 = 3;
constexpr int E8 = 60, G8 = 62, C8 = 58, F8 = 61, D8 = 59;

constexpr Bitboard WHITE_OO_RIGHT  = 1ULL << 7;   // h1
constexpr Bitboard WHITE_OOO_RIGHT = 1ULL << 0;   // a1
constexpr Bitboard BLACK_OO_RIGHT  = 1ULL << 63;  // h8
constexpr Bitboard BLACK_OOO_RIGHT = 1ULL << 56;  // a8

constexpr Bitboard WHITE_OO_PATH  = (1ULL << F1) | (1ULL << G1);
constexpr Bitboard WHITE_OOO_PATH = (1ULL << 1) | (1ULL << C1) | (1ULL << D1);  // b1, c1, d1
constexpr Bitboard BLACK_OO_PATH  = (1ULL << F8) | (1ULL << G8);
constexpr Bitboard BLACK_OOO_PATH = (1ULL << 57) | (1ULL << C8) | (1ULL << D8); // b8, c8, d8

template <Color turn>
std::vector<Move32> generate_moves(const Board& board) {
    std::vector<Move32> moves;

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
            moves.emplace_back(from_sq, to_sq, Piece::Queen);
            moves.emplace_back(from_sq, to_sq, Piece::Rook);
            moves.emplace_back(from_sq, to_sq, Piece::Bishop);
            moves.emplace_back(from_sq, to_sq, Piece::Knight);
        }

        // Capture promotions
        Bitboard captures = PAWN_ATTACKS[(int)turn][from_sq] & enemy_occupied;
        for (Bitboard cap_bb = captures; cap_bb; cap_bb &= cap_bb - 1) {
            int to_sq = lsb_index(cap_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.emplace_back(from_sq, to_sq, Piece::Queen, captured_piece);
            moves.emplace_back(from_sq, to_sq, Piece::Rook, captured_piece);
            moves.emplace_back(from_sq, to_sq, Piece::Bishop, captured_piece);
            moves.emplace_back(from_sq, to_sq, Piece::Knight, captured_piece);
        }
    }

    // Normal pawns (no promotions possible)
    for (Bitboard bb = normal_pawns; bb; bb &= bb - 1) {
        int from_sq = lsb_index(bb);

        // Single push
        Bitboard single_move = PAWN_MOVES_ONE[(int)turn][from_sq] & not_occupied;
        if (single_move) {
            int to_sq = lsb_index(single_move);
            moves.emplace_back(from_sq, to_sq);

            // Double push
            Bitboard double_move = PAWN_MOVES_TWO[(int)turn][from_sq] & not_occupied;
            if (double_move) {
                int to_sq_double = lsb_index(double_move);
                moves.emplace_back(from_sq, to_sq_double);
            }
        }

        // Captures (including en passant)
        Bitboard captures = PAWN_ATTACKS[(int)turn][from_sq] & (enemy_occupied | board.en_passant);
        for (Bitboard cap_bb = captures; cap_bb; cap_bb &= cap_bb - 1) {
            int to_sq = lsb_index(cap_bb);
            bool is_ep = square_bb(to_sq) & board.en_passant;
            Piece captured_piece = is_ep ? Piece::Pawn : board.pieces_on_square[to_sq];
            Move32 m(from_sq, to_sq, Piece::None, captured_piece);
            if (is_ep) m.set_en_passant();
            moves.push_back(m);
        }
    }

    Bitboard knights_bb = board.pieces[(int)turn][(int)Piece::Knight];
    for (Bitboard from_bb = knights_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard knight_moves = KNIGHT_MOVES[from_sq] & (~board.occupied[(int)turn]);
        for (Bitboard to_bb = knight_moves; to_bb; to_bb &= to_bb - 1) {
            int to_sq = lsb_index(to_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.emplace_back(from_sq, to_sq, Piece::None, captured_piece);
        }
    }

    Bitboard rooks_bb = board.pieces[(int)turn][(int)Piece::Rook];
    for (Bitboard from_bb = rooks_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard rook_moves = get_rook_attacks(from_sq, board.all_occupied) & (~board.occupied[(int)turn]);
        for (Bitboard to_bb = rook_moves; to_bb; to_bb &= to_bb - 1) {
            int to_sq = lsb_index(to_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.emplace_back(from_sq, to_sq, Piece::None, captured_piece);
        }
    }

    Bitboard bishops_bb = board.pieces[(int)turn][(int)Piece::Bishop];
    for (Bitboard from_bb = bishops_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard bishop_moves = get_bishop_attacks(from_sq, board.all_occupied) & (~board.occupied[(int)turn]);
        for (Bitboard to_bb = bishop_moves; to_bb; to_bb &= to_bb - 1) {
            int to_sq = lsb_index(to_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.emplace_back(from_sq, to_sq, Piece::None, captured_piece);
        }
    }

    Bitboard queens_bb = board.pieces[(int)turn][(int)Piece::Queen];
    for (Bitboard from_bb = queens_bb; from_bb; from_bb &= from_bb - 1) {
        int from_sq = lsb_index(from_bb);
        Bitboard queen_moves = get_queen_attacks(from_sq, board.all_occupied) & (~board.occupied[(int)turn]);
        for (Bitboard to_bb = queen_moves; to_bb; to_bb &= to_bb - 1) {
            int to_sq = lsb_index(to_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.emplace_back(from_sq, to_sq, Piece::None, captured_piece);
        }
    }

    {
        Bitboard king_bb = board.pieces[(int)turn][(int)Piece::King];
        int from_sq = lsb_index(king_bb);
        Bitboard king_moves = KING_MOVES[from_sq] & (~board.occupied[(int)turn]);
        for (Bitboard to_bb = king_moves; to_bb; to_bb &= to_bb - 1) {
            int to_sq = lsb_index(to_bb);
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.emplace_back(from_sq, to_sq, Piece::None, captured_piece);
        }

        // Castling
        constexpr Color enemy = (turn == Color::White) ? Color::Black : Color::White;

        if constexpr (turn == Color::White) {
            if (from_sq == E1) {
                // Kingside: rights + path empty + e1,f1,g1 not attacked
                if ((board.castling & WHITE_OO_RIGHT) &&
                    !(board.all_occupied & WHITE_OO_PATH) &&
                    !is_attacked<enemy>(E1, board) &&
                    !is_attacked<enemy>(F1, board) &&
                    !is_attacked<enemy>(G1, board)) {
                    Move32 m(E1, G1);
                    m.set_castling();
                    moves.push_back(m);
                }
                // Queenside: rights + path empty + e1,d1,c1 not attacked
                if ((board.castling & WHITE_OOO_RIGHT) &&
                    !(board.all_occupied & WHITE_OOO_PATH) &&
                    !is_attacked<enemy>(E1, board) &&
                    !is_attacked<enemy>(D1, board) &&
                    !is_attacked<enemy>(C1, board)) {
                    Move32 m(E1, C1);
                    m.set_castling();
                    moves.push_back(m);
                }
            }
        } else {
            if (from_sq == E8) {
                // Kingside
                if ((board.castling & BLACK_OO_RIGHT) &&
                    !(board.all_occupied & BLACK_OO_PATH) &&
                    !is_attacked<enemy>(E8, board) &&
                    !is_attacked<enemy>(F8, board) &&
                    !is_attacked<enemy>(G8, board)) {
                    Move32 m(E8, G8);
                    m.set_castling();
                    moves.push_back(m);
                }
                // Queenside
                if ((board.castling & BLACK_OOO_RIGHT) &&
                    !(board.all_occupied & BLACK_OOO_PATH) &&
                    !is_attacked<enemy>(E8, board) &&
                    !is_attacked<enemy>(D8, board) &&
                    !is_attacked<enemy>(C8, board)) {
                    Move32 m(E8, C8);
                    m.set_castling();
                    moves.push_back(m);
                }
            }
        }
    }

    return moves;
}

std::vector<Move32> generate_moves(const Board& board) {
    if (board.turn == Color::White) {
        return generate_moves<Color::White>(board);
    } else {
        return generate_moves<Color::Black>(board);
    }
}

// Castling rook squares
constexpr int A1 = 0, H1 = 7, A8 = 56, H8 = 63;

// Castling rights masks for each corner
constexpr Bitboard CASTLING_MASK[64] = {
    ~(1ULL << A1), ~0ULL, ~0ULL, ~0ULL, ~((1ULL << A1) | (1ULL << H1)), ~0ULL, ~0ULL, ~(1ULL << H1),  // rank 1
    ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL,
    ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL,
    ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL,
    ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL,
    ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL,
    ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL,
    ~(1ULL << A8), ~0ULL, ~0ULL, ~0ULL, ~((1ULL << A8) | (1ULL << H8)), ~0ULL, ~0ULL, ~(1ULL << H8)   // rank 8
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
    move.set_undo_info(board.castling, board.en_passant);

    // Flip turn
    board.turn = enemy;

    // Clear en passant (will be set below if this is a double pawn push)
    board.en_passant = 0;

    // Move the piece
    board.pieces_on_square[to] = to_piece;
    board.pieces_on_square[from] = Piece::None;

    board.occupied[(int)turn] &= ~square_bb(from);
    board.occupied[(int)turn] |= square_bb(to);
    board.pieces[(int)turn][(int)piece] &= ~square_bb(from);
    board.pieces[(int)turn][(int)to_piece] |= square_bb(to);

    // Handle captures
    if (captured != Piece::None) {
        if (move.is_en_passant()) {
            // En passant: captured pawn is behind the destination square
            int captured_sq = (turn == Color::White) ? to - 8 : to + 8;
            board.pieces_on_square[captured_sq] = Piece::None;
            board.occupied[(int)enemy] &= ~square_bb(captured_sq);
            board.pieces[(int)enemy][(int)Piece::Pawn] &= ~square_bb(captured_sq);
        } else {
            board.occupied[(int)enemy] &= ~square_bb(to);
            board.pieces[(int)enemy][(int)captured] &= ~square_bb(to);
        }
    }

    // Handle castling
    if (move.is_castling()) {
        int rook_from, rook_to;
        if (to == G1) { rook_from = H1; rook_to = F1; }
        else if (to == C1) { rook_from = A1; rook_to = D1; }
        else if (to == G8) { rook_from = H8; rook_to = F8; }
        else { rook_from = A8; rook_to = D8; }  // C8

        board.pieces_on_square[rook_to] = Piece::Rook;
        board.pieces_on_square[rook_from] = Piece::None;
        board.occupied[(int)turn] &= ~square_bb(rook_from);
        board.occupied[(int)turn] |= square_bb(rook_to);
        board.pieces[(int)turn][(int)Piece::Rook] &= ~square_bb(rook_from);
        board.pieces[(int)turn][(int)Piece::Rook] |= square_bb(rook_to);
    }

    // Update all_occupied
    board.all_occupied = board.occupied[0] | board.occupied[1];

    // Update castling rights
    board.castling &= CASTLING_MASK[from] & CASTLING_MASK[to];

    // Set en passant target if double pawn push
    if (piece == Piece::Pawn) {
        int diff = to - from;
        if (diff == 16) {  // White double push
            board.en_passant = square_bb(from + 8);
        } else if (diff == -16) {  // Black double push
            board.en_passant = square_bb(from - 8);
        }
    }
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

    // Restore castling rights and en passant
    board.castling = move.prev_castling_bb();
    board.en_passant = move.prev_ep_bb();

    // Determine original piece (pawn if this was a promotion)
    const Piece to_piece = board.pieces_on_square[to];
    const Piece piece = (promotion != Piece::None) ? Piece::Pawn : to_piece;

    // Move piece back
    board.pieces_on_square[from] = piece;
    board.pieces_on_square[to] = Piece::None;

    board.occupied[(int)turn] |= square_bb(from);
    board.occupied[(int)turn] &= ~square_bb(to);
    board.pieces[(int)turn][(int)piece] |= square_bb(from);
    board.pieces[(int)turn][(int)to_piece] &= ~square_bb(to);

    // Restore captured piece
    if (captured != Piece::None) {
        if (move.is_en_passant()) {
            // En passant: restore pawn behind destination square
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

    // Undo castling rook move
    if (move.is_castling()) {
        int rook_from, rook_to;
        if (to == G1) { rook_from = H1; rook_to = F1; }
        else if (to == C1) { rook_from = A1; rook_to = D1; }
        else if (to == G8) { rook_from = H8; rook_to = F8; }
        else { rook_from = A8; rook_to = D8; }  // C8

        board.pieces_on_square[rook_from] = Piece::Rook;
        board.pieces_on_square[rook_to] = Piece::None;
        board.occupied[(int)turn] |= square_bb(rook_from);
        board.occupied[(int)turn] &= ~square_bb(rook_to);
        board.pieces[(int)turn][(int)Piece::Rook] |= square_bb(rook_from);
        board.pieces[(int)turn][(int)Piece::Rook] &= ~square_bb(rook_to);
    }

    // Update all_occupied
    board.all_occupied = board.occupied[0] | board.occupied[1];
}

bool is_attacked(int square, Color attacker, const Board& board) {
    if (attacker == Color::White) {
        return is_attacked<Color::White>(square, board);
    } else {
        return is_attacked<Color::Black>(square, board);
    }
}

bool in_check(const Board& board) {
    Color us = board.turn;
    Color them = opposite(us);
    Bitboard king_bb = board.pieces[(int)us][(int)Piece::King];
    int king_sq = lsb_index(king_bb);
    return is_attacked(king_sq, them, board);
}
