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

            // Double push (PAWN_MOVES_TWO is zero except from starting rank)
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
            Piece captured_piece = board.pieces_on_square[to_sq];
            moves.emplace_back(from_sq, to_sq, Piece::None, captured_piece);
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
    }

    return moves;
}

template std::vector<Move32> generate_moves<Color::White>(const Board&);
template std::vector<Move32> generate_moves<Color::Black>(const Board&);
