#include "eval.hpp"

// Material values (centipawns)
constexpr int PIECE_VALUES[] = {
    100,   // Pawn
    320,   // Knight
    330,   // Bishop
    500,   // Rook
    900,   // Queen
    0      // King
};

int evaluate(const Board& board) {
    int score = 0;

    for (int piece = 0; piece < 5; ++piece) {
        score += PIECE_VALUES[piece] * popcount(board.pieces[0][piece]);  // White
        score -= PIECE_VALUES[piece] * popcount(board.pieces[1][piece]);  // Black
    }

    return (board.turn == Color::White) ? score : -score;
}
