#include "tests.hpp"
#include "board.hpp"
#include "search.hpp"
#include "move.hpp"
#include "ttable.hpp"
#include "zobrist.hpp"
#include "eval.hpp"
#include "perft.hpp"
#include <iostream>
#include <cmath>
#include <sstream>

// Helper to apply UCI move
static void apply_move(Board& board, const std::string& uci) {
    Move32 move = parse_uci_move(uci, board);
    make_move(board, move);
}

// Helper to compare two boards for equality
static bool boards_equal(const Board& a, const Board& b) {
    if (a.turn != b.turn) return false;
    if (a.ep_file != b.ep_file) return false;
    if (a.castling != b.castling) return false;
    if (a.halfmove_clock != b.halfmove_clock) return false;
    if (a.all_occupied != b.all_occupied) return false;
    if (a.hash != b.hash) return false;
    for (int c = 0; c < 2; c++) {
        if (a.occupied[c] != b.occupied[c]) return false;
        if (a.king_sq[c] != b.king_sq[c]) return false;
        for (int p = 0; p < 6; p++) {
            if (a.pieces[c][p] != b.pieces[c][p]) return false;
        }
    }
    for (int sq = 0; sq < 64; sq++) {
        if (a.pieces_on_square[sq] != b.pieces_on_square[sq]) return false;
    }
    return true;
}

// Helper to check board invariants
static bool check_board_invariants(const Board& board, std::string& error) {
    // Check occupied bitboards match piece bitboards
    for (int color = 0; color < 2; color++) {
        Bitboard expected_occupied = 0;
        for (int piece = 0; piece < 6; piece++) {
            expected_occupied |= board.pieces[color][piece];
        }
        if (board.occupied[color] != expected_occupied) {
            error = "occupied[" + std::to_string(color) + "] doesn't match pieces";
            return false;
        }
    }

    // Check all_occupied matches occupied[0] | occupied[1]
    if (board.all_occupied != (board.occupied[0] | board.occupied[1])) {
        error = "all_occupied doesn't match occupied[0] | occupied[1]";
        return false;
    }

    // Check pieces_on_square matches bitboards
    for (int sq = 0; sq < 64; sq++) {
        Bitboard sq_bb = square_bb(sq);
        Piece expected = Piece::None;
        for (int color = 0; color < 2; color++) {
            for (int piece = 0; piece < 6; piece++) {
                if (board.pieces[color][piece] & sq_bb) {
                    if (expected != Piece::None) {
                        error = "multiple pieces on square " + std::to_string(sq);
                        return false;
                    }
                    expected = static_cast<Piece>(piece);
                }
            }
        }
        if (board.pieces_on_square[sq] != expected) {
            error = "pieces_on_square[" + std::to_string(sq) + "] mismatch";
            return false;
        }
    }

    // Check king positions
    for (int color = 0; color < 2; color++) {
        Bitboard king_bb = board.pieces[color][(int)Piece::King];
        if (popcount(king_bb) != 1) {
            error = "expected exactly 1 king for color " + std::to_string(color);
            return false;
        }
        if (board.king_sq[color] != lsb_index(king_bb)) {
            error = "king_sq[" + std::to_string(color) + "] doesn't match king bitboard";
            return false;
        }
    }

    // Check no pawns on rank 1 or rank 8
    Bitboard rank_1_8 = 0xFFULL | (0xFFULL << 56);
    if ((board.pieces[0][(int)Piece::Pawn] | board.pieces[1][(int)Piece::Pawn]) & rank_1_8) {
        error = "pawns on rank 1 or 8";
        return false;
    }

    // Check hash consistency
    if (board.hash != compute_hash(board)) {
        error = "hash doesn't match compute_hash()";
        return false;
    }

    return true;
}

// Helper to compare FEN strings (ignoring fullmove number, which we don't track)
static bool fen_equal(const std::string& a, const std::string& b) {
    // Compare first 5 fields (position, turn, castling, ep, halfmove)
    // Skip fullmove number (6th field)
    std::istringstream ss_a(a), ss_b(b);
    std::string field_a, field_b;
    for (int i = 0; i < 5; i++) {
        if (!(ss_a >> field_a) || !(ss_b >> field_b)) return false;
        if (field_a != field_b) return false;
    }
    return true;
}

// =============================================================================
// FEN TESTS
// =============================================================================

// Test: FEN roundtrip for starting position
static bool test_fen_roundtrip_start() {
    std::string fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    Board board(fen);
    std::string result = board.to_fen();
    return fen_equal(result, fen);
}

// Test: FEN roundtrip for position with en passant
static bool test_fen_roundtrip_ep() {
    std::string fen = "rnbqkbnr/pppp1ppp/8/4pP2/8/8/PPPPP1PP/RNBQKBNR w KQkq e6 0 3";
    Board board(fen);
    std::string result = board.to_fen();
    return fen_equal(result, fen);
}

// Test: FEN roundtrip for position with partial castling rights
static bool test_fen_roundtrip_castling() {
    std::string fen = "r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w Kq - 5 10";
    Board board(fen);
    std::string result = board.to_fen();
    return fen_equal(result, fen);
}

// Test: FEN roundtrip for complex middlegame position
static bool test_fen_roundtrip_complex() {
    std::string fen = "r1bqk2r/ppp2ppp/2n2n2/2bpp3/2B1P3/3P1N2/PPP2PPP/RNBQK2R w KQkq d6 0 6";
    Board board(fen);
    std::string result = board.to_fen();
    return fen_equal(result, fen);
}

// =============================================================================
// MAKE/UNMAKE TESTS
// =============================================================================

// Test: Make/unmake restores board state for all legal moves from starting position
static bool test_make_unmake_start_position() {
    Board board;
    Board original = board;

    MoveList moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        Move32 move = moves[i];
        make_move(board, move);
        unmake_move(board, move);
        if (!boards_equal(board, original)) {
            std::cerr << "  make/unmake failed for move " << move.to_uci() << std::endl;
            return false;
        }
    }
    return true;
}

// Test: Make/unmake works correctly for captures
static bool test_make_unmake_captures() {
    // Position with many capture possibilities
    Board board("r1bqkbnr/pppp1ppp/2n5/4p3/3PP3/5N2/PPP2PPP/RNBQKB1R b KQkq d3 0 3");
    Board original = board;

    MoveList moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        Move32 move = moves[i];
        make_move(board, move);
        unmake_move(board, move);
        if (!boards_equal(board, original)) {
            std::cerr << "  make/unmake failed for move " << move.to_uci() << std::endl;
            return false;
        }
    }
    return true;
}

// Test: Make/unmake works for complex position with many move types
static bool test_make_unmake_complex() {
    // Position with castling, captures, en passant possible
    Board board("r3k2r/pppqbppp/2npbn2/4p3/2B1P3/2NP1N2/PPPBQPPP/R3K2R w KQkq - 4 8");
    Board original = board;

    MoveList moves = generate_moves(board);
    for (int i = 0; i < moves.size; i++) {
        Move32 move = moves[i];
        make_move(board, move);
        unmake_move(board, move);
        if (!boards_equal(board, original)) {
            std::cerr << "  make/unmake failed for move " << move.to_uci() << std::endl;
            return false;
        }
    }
    return true;
}

// =============================================================================
// ZOBRIST HASH TESTS
// =============================================================================

// Test: Hash is consistent with compute_hash after construction
static bool test_hash_after_construction() {
    Board board;
    return board.hash == compute_hash(board);
}

// Test: Hash is consistent after multiple moves
static bool test_hash_after_moves() {
    Board board;

    // Play e4 e5 Nf3 Nc6
    apply_move(board, "e2e4");
    if (board.hash != compute_hash(board)) return false;

    apply_move(board, "e7e5");
    if (board.hash != compute_hash(board)) return false;

    apply_move(board, "g1f3");
    if (board.hash != compute_hash(board)) return false;

    apply_move(board, "b8c6");
    if (board.hash != compute_hash(board)) return false;

    return true;
}

// Test: Hash changes when any component changes
static bool test_hash_changes() {
    Board board1("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    Board board2("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");  // Different turn
    Board board3("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w Kq - 0 1");    // Different castling
    Board board4("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e3 0 1"); // Different ep

    if (board1.hash == board2.hash) return false;  // Turn should affect hash
    if (board1.hash == board3.hash) return false;  // Castling should affect hash
    if (board1.hash == board4.hash) return false;  // EP should affect hash

    return true;
}

// =============================================================================
// CASTLING TESTS
// =============================================================================

// Test: White kingside castling
static bool test_white_kingside_castle() {
    Board board("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
    apply_move(board, "e1g1");

    // King should be on g1, rook on f1
    if (board.king_sq[0] != 6) return false;  // g1 = 6
    if (!(board.pieces[0][(int)Piece::Rook] & square_bb(5))) return false;  // f1 = 5

    // Castling rights should be updated (no more white castling)
    if (board.castling & 0b0011) return false;

    std::string error;
    return check_board_invariants(board, error);
}

// Test: White queenside castling
static bool test_white_queenside_castle() {
    Board board("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
    apply_move(board, "e1c1");

    // King should be on c1, rook on d1
    if (board.king_sq[0] != 2) return false;  // c1 = 2
    if (!(board.pieces[0][(int)Piece::Rook] & square_bb(3))) return false;  // d1 = 3

    std::string error;
    return check_board_invariants(board, error);
}

// Test: Black kingside castling
static bool test_black_kingside_castle() {
    Board board("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R b KQkq - 0 1");
    apply_move(board, "e8g8");

    // King should be on g8, rook on f8
    if (board.king_sq[1] != 62) return false;  // g8 = 62
    if (!(board.pieces[1][(int)Piece::Rook] & square_bb(61))) return false;  // f8 = 61

    std::string error;
    return check_board_invariants(board, error);
}

// Test: Black queenside castling
static bool test_black_queenside_castle() {
    Board board("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R b KQkq - 0 1");
    apply_move(board, "e8c8");

    // King should be on c8, rook on d8
    if (board.king_sq[1] != 58) return false;  // c8 = 58
    if (!(board.pieces[1][(int)Piece::Rook] & square_bb(59))) return false;  // d8 = 59

    std::string error;
    return check_board_invariants(board, error);
}

// Test: Castling rights lost when king moves
static bool test_castling_rights_lost_king_move() {
    Board board("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
    u8 orig_castling = board.castling;
    apply_move(board, "e1f1");  // King moves (not castling)

    // White castling rights should be lost
    if (board.castling & 0b0011) return false;
    // Black castling rights should remain
    if ((board.castling & 0b1100) != (orig_castling & 0b1100)) return false;

    return true;
}

// Test: Castling rights lost when rook moves
static bool test_castling_rights_lost_rook_move() {
    Board board("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");
    apply_move(board, "h1g1");  // Kingside rook moves

    // White kingside should be lost, queenside should remain
    if (board.castling & 0b0010) return false;   // K should be gone
    if (!(board.castling & 0b0001)) return false; // Q should remain

    return true;
}

// Test: Castling rights lost when rook is captured
static bool test_castling_rights_lost_rook_captured() {
    Board board("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2q w Qq - 0 1");
    // Black queen on h1 can be taken, but white's h-rook is gone
    // Actually let's use a position where a rook gets captured
    Board board2("r3k2r/pppppppQ/8/8/8/8/PPPPPPP1/R3K2R b KQq - 0 1");
    // White queen on h7 can take h8 rook
    Board board3("r3k2r/pppppppQ/8/8/8/8/PPPPPPP1/R3K2R w KQq - 0 1");
    apply_move(board3, "h7h8");  // Qxh8

    // Black kingside castling should be lost
    if (board3.castling & 0b1000) return false;

    return true;
}

// =============================================================================
// EN PASSANT TESTS
// =============================================================================

// Test: En passant capture removes correct pawn
static bool test_en_passant_capture() {
    Board board("rnbqkbnr/pppp1ppp/8/4pP2/8/8/PPPPP1PP/RNBQKBNR w KQkq e6 0 3");

    apply_move(board, "f5e6");  // fxe6 en passant

    // e5 pawn should be removed (not e6)
    Bitboard e5_bb = square_bb(36);  // e5 = 36
    if (board.pieces[1][(int)Piece::Pawn] & e5_bb) return false;

    // White pawn should be on e6
    Bitboard e6_bb = square_bb(44);  // e6 = 44
    if (!(board.pieces[0][(int)Piece::Pawn] & e6_bb)) return false;

    // Halfmove clock should reset
    if (board.halfmove_clock != 0) return false;

    std::string error;
    return check_board_invariants(board, error);
}

// Test: En passant file only set after double pawn push
static bool test_en_passant_file_set() {
    Board board;
    apply_move(board, "e2e4");  // Double push

    // EP file should be set to e-file (4)
    if (board.ep_file != 4) return false;

    apply_move(board, "e7e6");  // Single push
    // EP file should be cleared
    if (board.ep_file != 8) return false;

    return true;
}

// =============================================================================
// PROMOTION TESTS
// =============================================================================

// Test: Pawn promotes to queen
static bool test_promotion_queen() {
    Board board("8/P7/8/8/8/8/8/4K2k w - - 0 1");
    apply_move(board, "a7a8q");

    // Should be a queen on a8
    Bitboard a8_bb = square_bb(56);
    if (!(board.pieces[0][(int)Piece::Queen] & a8_bb)) return false;
    // No pawn on a8
    if (board.pieces[0][(int)Piece::Pawn] & a8_bb) return false;

    std::string error;
    return check_board_invariants(board, error);
}

// Test: Pawn promotes to knight
static bool test_promotion_knight() {
    Board board("8/P7/8/8/8/8/8/4K2k w - - 0 1");
    apply_move(board, "a7a8n");

    Bitboard a8_bb = square_bb(56);
    if (!(board.pieces[0][(int)Piece::Knight] & a8_bb)) return false;

    std::string error;
    return check_board_invariants(board, error);
}

// Test: Pawn promotes to rook
static bool test_promotion_rook() {
    Board board("8/P7/8/8/8/8/8/4K2k w - - 0 1");
    apply_move(board, "a7a8r");

    Bitboard a8_bb = square_bb(56);
    if (!(board.pieces[0][(int)Piece::Rook] & a8_bb)) return false;

    std::string error;
    return check_board_invariants(board, error);
}

// Test: Pawn promotes to bishop
static bool test_promotion_bishop() {
    Board board("8/P7/8/8/8/8/8/4K2k w - - 0 1");
    apply_move(board, "a7a8b");

    Bitboard a8_bb = square_bb(56);
    if (!(board.pieces[0][(int)Piece::Bishop] & a8_bb)) return false;

    std::string error;
    return check_board_invariants(board, error);
}

// Test: Capture promotion
static bool test_promotion_capture() {
    Board board("1n6/P7/8/8/8/8/8/4K2k w - - 0 1");
    apply_move(board, "a7b8q");

    Bitboard b8_bb = square_bb(57);
    if (!(board.pieces[0][(int)Piece::Queen] & b8_bb)) return false;
    // Black knight should be gone
    if (board.pieces[1][(int)Piece::Knight] & b8_bb) return false;

    std::string error;
    return check_board_invariants(board, error);
}

// =============================================================================
// BOARD INVARIANT TESTS
// =============================================================================

// Test: Board invariants hold after construction
static bool test_invariants_after_construction() {
    Board board;
    std::string error;
    if (!check_board_invariants(board, error)) {
        std::cerr << "  " << error << std::endl;
        return false;
    }
    return true;
}

// Test: Board invariants hold after several moves
static bool test_invariants_after_moves() {
    Board board;
    std::string error;

    // Play a game
    std::vector<std::string> moves = {
        "e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6",
        "e1g1", "f6e4", "d2d4", "b7b5", "a4b3", "d7d5", "d4e5", "c8e6"
    };

    for (const auto& uci : moves) {
        apply_move(board, uci);
        if (!check_board_invariants(board, error)) {
            std::cerr << "  after " << uci << ": " << error << std::endl;
            return false;
        }
    }
    return true;
}

// =============================================================================
// HALFMOVE CLOCK TESTS
// =============================================================================

// Test: Halfmove clock resets on pawn move
static bool test_halfmove_resets_pawn() {
    Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 10 1");
    apply_move(board, "e2e4");
    return board.halfmove_clock == 0;
}

// Test: Halfmove clock resets on capture
static bool test_halfmove_resets_capture() {
    Board board("rnbqkbnr/ppp1pppp/8/3p4/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 5 3");
    apply_move(board, "e4d5");  // exd5
    return board.halfmove_clock == 0;
}

// Test: Halfmove clock increments on quiet move
static bool test_halfmove_increments() {
    Board board("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 5 1");
    apply_move(board, "e1f1");  // King moves (not pawn, not capture)
    return board.halfmove_clock == 6;
}

// =============================================================================
// PERFT TESTS (correctness of move generation)
// =============================================================================

// Test: Perft depth 1 from starting position
static bool test_perft_start_d1() {
    Board board;
    PerftTable pt(1);
    u64 nodes = perft(board, 1, &pt);
    return nodes == 20;  // 20 legal moves from starting position
}

// Test: Perft depth 2 from starting position
static bool test_perft_start_d2() {
    Board board;
    PerftTable pt(1);
    u64 nodes = perft(board, 2, &pt);
    return nodes == 400;
}

// Test: Perft depth 3 from starting position
static bool test_perft_start_d3() {
    Board board;
    PerftTable pt(1);
    u64 nodes = perft(board, 3, &pt);
    return nodes == 8902;
}

// Test: Perft depth 4 from starting position
static bool test_perft_start_d4() {
    Board board;
    PerftTable pt(1);
    u64 nodes = perft(board, 4, &pt);
    return nodes == 197281;
}

// Test: Perft for Kiwipete position (complex position with many edge cases)
static bool test_perft_kiwipete_d1() {
    Board board("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 1, &pt);
    return nodes == 48;
}

// Test: Perft for Kiwipete position depth 2
static bool test_perft_kiwipete_d2() {
    Board board("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 2, &pt);
    return nodes == 2039;
}

// Test: Perft for en passant position
static bool test_perft_ep_position() {
    Board board("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 3, &pt);
    return nodes == 2812;
}

// Test: Perft for promotion position
static bool test_perft_promotion_position() {
    Board board("n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1");
    PerftTable pt(1);
    u64 nodes = perft(board, 3, &pt);
    return nodes == 9483;
}

// =============================================================================
// EVALUATION SANITY TESTS
// =============================================================================

// Test: Equal material evaluates near zero
static bool test_eval_equal_material() {
    Board board;  // Starting position
    int score = evaluate(board);
    // Should be close to 0 (within 100 centipawns)
    return std::abs(score) < 100;
}

// Test: Material advantage gives positive score
static bool test_eval_material_advantage() {
    Board board("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKB1R w KQkq - 0 1");  // White missing knight
    Board board2("rnbqkb1r/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"); // Black missing knight

    int score1 = evaluate(board);   // White is down
    int score2 = evaluate(board2);  // Black is down (White advantage)

    // White down material should be negative (from white's perspective)
    // But evaluate() returns from side-to-move perspective, so both are white's turn
    return score1 < 0 && score2 > 0;
}

// Test: Queen advantage is large
static bool test_eval_queen_advantage() {
    Board board("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");  // Black missing queen
    int score = evaluate(board);
    // Should be at least 800 centipawns (queen ~900)
    return score > 800;
}

// Helper to count legal moves (generate_moves returns pseudo-legal moves)
static int count_legal_moves(Board& board) {
    MoveList moves = generate_moves(board);
    int legal = 0;
    for (int i = 0; i < moves.size; i++) {
        make_move(board, moves[i]);
        if (!is_illegal(board)) {
            legal++;
        }
        unmake_move(board, moves[i]);
    }
    return legal;
}

// =============================================================================
// CHECKMATE/STALEMATE TESTS
// =============================================================================

// Test: Checkmate detection (Scholar's mate position)
static bool test_checkmate_detection() {
    Board board("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3");
    // White is checkmated
    int legal_moves = count_legal_moves(board);
    // No legal moves and king in check = checkmate
    if (legal_moves != 0) return false;
    return is_attacked(board.king_sq[0], Color::Black, board);
}

// Test: Stalemate detection
static bool test_stalemate_detection() {
    // Black king on h8, white queen on f7, white king on g6
    // h8 is not attacked, but all escape squares (g7, g8, h7) are attacked by queen
    Board board("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    int legal_moves = count_legal_moves(board);

    // Verify no legal moves
    if (legal_moves != 0) return false;

    // Verify not in check (would be stalemate, not checkmate)
    return !is_attacked(board.king_sq[1], Color::White, board);
}

// Test: Engine finds mate in 1
static bool test_find_mate_in_1() {
    Board board("k7/8/1K6/8/8/8/8/1Q6 w - - 0 1");
    TTable tt(16);
    auto result = search(board, tt, 200);

    // Should find Qa2# or Qb8# (mate score)
    // Mate scores are typically very high positive values
    return result.score > 9000;
}

// Test: Engine finds mate in 2
static bool test_find_mate_in_2() {
    // A classic mate in 2 position
    Board board("2bqkbn1/2pppp2/np2N3/r3P1p1/p2N2B1/5Q2/PPPPPP1P/RNB1K2R w KQ - 0 1");
    TTable tt(16);
    auto result = search(board, tt, 500);

    // Should find Qf7# or similar mate
    return result.score > 9000;
}

// =============================================================================
// TRANSPOSITION TABLE TESTS
// =============================================================================

// Test: TTable store and probe basic functionality
static bool test_ttable_store_probe() {
    TTable tt(1);  // 1 MB

    Board board;
    Move32 move(12, 28);  // e2e4

    // Store an exact score
    tt.store(board.hash, 5, 100, TT_EXACT, move);

    // Probe should succeed with same depth
    int score;
    Move32 best_move;
    bool hit = tt.probe(board.hash, 5, -10000, 10000, score, best_move);

    if (!hit) return false;
    if (score != 100) return false;
    if (!best_move.same_move(move)) return false;

    return true;
}

// Test: TTable respects depth
static bool test_ttable_depth_check() {
    TTable tt(1);

    Board board;
    Move32 move(12, 28);

    // Store at depth 3
    tt.store(board.hash, 3, 100, TT_EXACT, move);

    // Probe at depth 5 should fail (stored depth too shallow)
    int score;
    Move32 best_move;
    bool hit = tt.probe(board.hash, 5, -10000, 10000, score, best_move);

    return !hit;  // Should NOT hit because depth is insufficient
}

// Test: TTable returns best move even without score hit
static bool test_ttable_returns_best_move() {
    TTable tt(1);

    Board board;
    Move32 move(12, 28);

    // Store at depth 3
    tt.store(board.hash, 3, 100, TT_EXACT, move);

    // Probe at depth 5 - should fail for cutoff but still get best_move
    int score;
    Move32 best_move;
    tt.probe(board.hash, 5, -10000, 10000, score, best_move);

    // best_move should still be set (for move ordering)
    return best_move.same_move(move);
}

// Test: Engine finds perpetual check when losing
// Position where White is down a queen but can force perpetual
static bool test_perpetual_saves_game() {
    // White: Kb1, Qc3
    // Black: Ka8, Qh1 (Black up a queen equivalent in material pressure)
    // White can perpetual with Qa5+ Kb8 Qb5+ Ka8 Qa5+ etc.
    Board board("k7/8/8/8/8/2Q5/8/1K5q w - - 0 1");

    // Play out: Qa5+ Kb8 Qb5+ Ka8 - now White should play Qa5+ again (repetition)
    apply_move(board, "c3a5");  // Qa5+
    apply_move(board, "a8b8");  // Kb8
    apply_move(board, "a5b5");  // Qb5+
    apply_move(board, "b8a8");  // Ka8

    // Now it's White's turn. Qa5+ creates repetition (draw)
    // Without repetition detection, White might play something else and lose
    TTable tt(64);
    auto result = search(board, tt, 500);

    // Engine should find perpetual (score near 0)
    // The move should be Qa5+ (b5a5) or any check that maintains the perpetual
    bool score_is_draw = std::abs(result.score) < 100;
    return score_is_draw;
}

// Test: Engine avoids repetition when winning
// KQ vs K endgame - engine must make progress, not shuffle
static bool test_avoid_repetition_when_winning() {
    Board board("8/8/8/4k3/8/8/1Q6/4K3 w - - 0 1");

    // Play some moves to set up potential repetition trap
    apply_move(board, "b2b5");  // Qb5+
    apply_move(board, "e5e6");  // Ke6
    apply_move(board, "b5b6");  // Qb6+
    apply_move(board, "e6e5");  // Ke5

    // Now if White plays Qb5+ again, that's getting close to repetition
    // Engine should find a winning line instead
    TTable tt(64);
    auto result = search(board, tt, 1000);

    // Score should be clearly winning (not near-draw)
    return result.score > 500;
}

// Test: 50-move rule detection
// Position with halfmove_clock = 100 should evaluate as draw
static bool test_50_move_rule_draw() {
    // KR vs K position, but halfmove_clock is 100 (drawn)
    Board board("8/8/8/4k3/8/8/8/4K2R w - - 100 51");

    TTable tt(64);
    auto result = search(board, tt, 500);

    // With halfmove_clock >= 100, position is drawn
    // Score should be near 0
    return std::abs(result.score) < 100;
}

// Test: Repetition detection respects irreversible moves
// After a capture, previous positions don't count for repetition
static bool test_repetition_resets_on_capture() {
    Board board("8/8/8/4k3/8/3p4/1Q6/4K3 w - - 0 1");

    // Set up a position, then capture resets history
    apply_move(board, "b2b5");  // Qb5+
    if (board.halfmove_clock != 1) return false;

    apply_move(board, "e5e6");  // Ke6
    if (board.halfmove_clock != 2) return false;

    apply_move(board, "b5d3");  // Qxd3 (capture!)

    // halfmove_clock should be 0 now after capture
    if (board.halfmove_clock != 0) {
        return false;
    }

    apply_move(board, "e6e5");  // Ke5
    if (board.halfmove_clock != 1) return false;

    // hash_sp should be 4 (4 moves made)
    if (board.hash_sp != 4) return false;

    // Verify board invariants hold
    std::string error;
    return check_board_invariants(board, error);
}

// Test: PV moves should all be valid
// Each move in the PV should have a piece on its from square belonging to the side to move
static bool test_pv_moves_valid() {
    // KQ vs K - search and validate entire PV
    Board board("8/8/4k3/8/8/8/1Q6/4K3 w - - 0 1");

    TTable tt(64);
    auto result = search(board, tt, 300);

    // Walk through the PV and validate each move
    Board test_board = board;
    for (int i = 0; i < result.pv_length; ++i) {
        Move32 move = result.pv[i];
        int from = move.from();

        // Check there's a piece on the from square
        Piece piece = test_board.pieces_on_square[from];
        if (piece == Piece::None) {
            std::cerr << "  PV move " << i << " (" << move.to_uci()
                      << "): no piece on from square" << std::endl;
            return false;
        }

        // Check the piece belongs to the side to move
        Bitboard from_bb = square_bb(from);
        if (!(test_board.occupied[(int)test_board.turn] & from_bb)) {
            std::cerr << "  PV move " << i << " (" << move.to_uci()
                      << "): piece doesn't belong to side to move" << std::endl;
            return false;
        }

        // Apply the move for the next iteration
        make_move(test_board, move);
    }

    return true;
}

// Test: Search tree repetition
// Engine should not play into an immediate repetition in the search tree
static bool test_search_tree_repetition() {
    // Simple position where both sides can shuffle
    Board board("8/8/8/8/8/k7/8/KQ6 w - - 0 1");

    // Set up moves that create a cycle
    apply_move(board, "b1b3");  // Qb3+
    apply_move(board, "a3a4");  // Ka4
    apply_move(board, "b3b4");  // Qb4+
    apply_move(board, "a4a3");  // Ka3

    // If White plays Qb3+ again, that's repetition
    // Engine should find a different winning move
    TTable tt(64);
    auto result = search(board, tt, 1000);

    // With good eval, engine should prefer winning over drawing
    return result.score > 300;
}

int run_draw_tests(int time_limit_ms, size_t mem_mb) {
    (void)time_limit_ms;  // Tests use their own timing
    (void)mem_mb;

    int failures = 0;

    auto run = [&](const char* name, bool (*test)()) {
        std::cout << "  " << name << "... " << std::flush;
        bool passed = test();
        std::cout << (passed ? "OK" : "FAIL") << std::endl;
        if (!passed) failures++;
    };

    std::cout << "=== FEN Tests ===" << std::endl;
    run("fen_roundtrip_start", test_fen_roundtrip_start);
    run("fen_roundtrip_ep", test_fen_roundtrip_ep);
    run("fen_roundtrip_castling", test_fen_roundtrip_castling);
    run("fen_roundtrip_complex", test_fen_roundtrip_complex);

    std::cout << "=== Make/Unmake Tests ===" << std::endl;
    run("make_unmake_start_position", test_make_unmake_start_position);
    run("make_unmake_captures", test_make_unmake_captures);
    run("make_unmake_complex", test_make_unmake_complex);

    std::cout << "=== Zobrist Hash Tests ===" << std::endl;
    run("hash_after_construction", test_hash_after_construction);
    run("hash_after_moves", test_hash_after_moves);
    run("hash_changes", test_hash_changes);

    std::cout << "=== Castling Tests ===" << std::endl;
    run("white_kingside_castle", test_white_kingside_castle);
    run("white_queenside_castle", test_white_queenside_castle);
    run("black_kingside_castle", test_black_kingside_castle);
    run("black_queenside_castle", test_black_queenside_castle);
    run("castling_rights_lost_king_move", test_castling_rights_lost_king_move);
    run("castling_rights_lost_rook_move", test_castling_rights_lost_rook_move);
    run("castling_rights_lost_rook_captured", test_castling_rights_lost_rook_captured);

    std::cout << "=== En Passant Tests ===" << std::endl;
    run("en_passant_capture", test_en_passant_capture);
    run("en_passant_file_set", test_en_passant_file_set);

    std::cout << "=== Promotion Tests ===" << std::endl;
    run("promotion_queen", test_promotion_queen);
    run("promotion_knight", test_promotion_knight);
    run("promotion_rook", test_promotion_rook);
    run("promotion_bishop", test_promotion_bishop);
    run("promotion_capture", test_promotion_capture);

    std::cout << "=== Board Invariant Tests ===" << std::endl;
    run("invariants_after_construction", test_invariants_after_construction);
    run("invariants_after_moves", test_invariants_after_moves);

    std::cout << "=== Halfmove Clock Tests ===" << std::endl;
    run("halfmove_resets_pawn", test_halfmove_resets_pawn);
    run("halfmove_resets_capture", test_halfmove_resets_capture);
    run("halfmove_increments", test_halfmove_increments);

    std::cout << "=== Perft Tests ===" << std::endl;
    run("perft_start_d1", test_perft_start_d1);
    run("perft_start_d2", test_perft_start_d2);
    run("perft_start_d3", test_perft_start_d3);
    run("perft_start_d4", test_perft_start_d4);
    run("perft_kiwipete_d1", test_perft_kiwipete_d1);
    run("perft_kiwipete_d2", test_perft_kiwipete_d2);
    run("perft_ep_position", test_perft_ep_position);
    run("perft_promotion_position", test_perft_promotion_position);

    std::cout << "=== Evaluation Tests ===" << std::endl;
    run("eval_equal_material", test_eval_equal_material);
    run("eval_material_advantage", test_eval_material_advantage);
    run("eval_queen_advantage", test_eval_queen_advantage);

    std::cout << "=== Checkmate/Stalemate Tests ===" << std::endl;
    run("checkmate_detection", test_checkmate_detection);
    run("stalemate_detection", test_stalemate_detection);
    run("find_mate_in_1", test_find_mate_in_1);
    run("find_mate_in_2", test_find_mate_in_2);

    std::cout << "=== Transposition Table Tests ===" << std::endl;
    run("ttable_store_probe", test_ttable_store_probe);
    run("ttable_depth_check", test_ttable_depth_check);
    run("ttable_returns_best_move", test_ttable_returns_best_move);

    std::cout << "=== Draw Detection Tests ===" << std::endl;
    run("perpetual_saves_game", test_perpetual_saves_game);
    run("avoid_repetition_when_winning", test_avoid_repetition_when_winning);
    run("50_move_rule_draw", test_50_move_rule_draw);
    run("repetition_resets_on_capture", test_repetition_resets_on_capture);
    run("search_tree_repetition", test_search_tree_repetition);

    std::cout << "=== PV Validation Tests ===" << std::endl;
    run("pv_moves_valid", test_pv_moves_valid);

    std::cout << "=== " << (failures == 0 ? "All tests passed!" : "Some tests FAILED")
              << " ===" << std::endl;

    return failures;
}
