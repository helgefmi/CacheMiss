#include "board.hpp"
#include "zobrist.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

Board::Board(std::string_view fen) {
    // Initialize all bitboards to zero
    std::ranges::fill(pieces[0], Bitboard(0));
    std::ranges::fill(pieces[1], Bitboard(0));
    std::ranges::fill(occupied, Bitboard(0));
    std::ranges::fill(pieces_on_square, Piece::None);
    all_occupied = Bitboard(0);
    ep_file = 8;  // 8 = no en passant
    castling = 0;
    halfmove_clock = 0;
    king_sq = {-1, -1};

    std::istringstream ss(fen.data());
    std::string position, active_color, castling_str, en_passant_str;
    int halfmove = 0, fullmove = 1;
    ss >> position >> active_color >> castling_str >> en_passant_str >> halfmove >> fullmove;
    halfmove_clock = static_cast<u8>(halfmove);

    // Parse piece positions (FEN starts at rank 8, file a)
    int rank = 7, file = 0;
    for (char c : position) {
        if (c == '/') {
            rank--;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
        } else if (auto piece = char_to_piece(c)) {
            auto [color, piece_type] = *piece;
            int sq = square_from_coords(file, rank);
            Bitboard bb = square_bb(sq);

            pieces[static_cast<int>(color)][static_cast<int>(piece_type)] |= bb;
            occupied[static_cast<int>(color)] |= bb;
            all_occupied |= bb;
            pieces_on_square[sq] = piece_type;
            if (piece_type == Piece::King) {
                king_sq[static_cast<int>(color)] = sq;
            }

            file++;
        }
    }

    // Parse active color
    turn = (active_color == "b") ? Color::Black : Color::White;

    // Parse castling rights (compact: bit0=wQ, bit1=wK, bit2=bQ, bit3=bK)
    for (char c : castling_str) {
        switch (c) {
            case 'K': castling |= 2; break;  // bit 1
            case 'Q': castling |= 1; break;  // bit 0
            case 'k': castling |= 8; break;  // bit 3
            case 'q': castling |= 4; break;  // bit 2
            default: break;
        }
    }

    // Parse en passant file (rank is always 3 for white target, 6 for black target)
    if (en_passant_str != "-" && en_passant_str.size() >= 2) {
        int file = en_passant_str[0] - 'a';
        if (file >= 0 && file < 8) {
            ep_file = file;
        }
    }

    // Compute initial Zobrist hash
    hash = compute_hash(*this);

    // Compute pawn-only hash for pawn structure cache
    pawn_key = 0;
    for (int c = 0; c < 2; ++c) {
        Bitboard pawns = pieces[c][(int)Piece::Pawn];
        while (pawns) {
            int sq = std::countr_zero(pawns);
            pawn_key ^= zobrist::pieces[c][(int)Piece::Pawn][sq];
            pawns &= pawns - 1;
        }
    }

    // Compute initial phase (Knight=1, Bishop=1, Rook=2, Queen=4, max 24)
    phase = 0;
    for (int c = 0; c < 2; ++c) {
        phase += popcount(pieces[c][(int)Piece::Knight]) * PHASE_VALUES[(int)Piece::Knight];
        phase += popcount(pieces[c][(int)Piece::Bishop]) * PHASE_VALUES[(int)Piece::Bishop];
        phase += popcount(pieces[c][(int)Piece::Rook]) * PHASE_VALUES[(int)Piece::Rook];
        phase += popcount(pieces[c][(int)Piece::Queen]) * PHASE_VALUES[(int)Piece::Queen];
    }
    if (phase > 24) phase = 24;
}

void Board::print() const {
    constexpr std::string_view piece_chars = "PNBRQKpnbrqk";

    auto piece_at = [&](int sq) -> char {
        if (!(all_occupied & square_bb(sq))) return '.';

        for (int color = 0; color < 2; ++color) {
            if (!(occupied[color] & square_bb(sq))) continue;
            for (int piece = 0; piece < 6; ++piece) {
                if (pieces[color][piece] & square_bb(sq)) {
                    return piece_chars[color * 6 + piece];
                }
            }
        }
        return '?';
    };

    std::cout << "\n  +---+---+---+---+---+---+---+---+\n";
    for (int rank = 7; rank >= 0; --rank) {
        std::cout << rank + 1 << " |";
        for (int file = 0; file < 8; ++file) {
            std::cout << ' ' << piece_at(square_from_coords(file, rank)) << " |";
        }
        std::cout << "\n  +---+---+---+---+---+---+---+---+\n";
    }
    std::cout << "    a   b   c   d   e   f   g   h\n\n";

    std::cout << "Turn: " << (turn == Color::White ? "White" : "Black") << '\n';

    std::cout << "Castling: ";
    if (castling == 0) std::cout << "-";
    else {
        if (castling & 2) std::cout << 'K';  // bit 1
        if (castling & 1) std::cout << 'Q';  // bit 0
        if (castling & 8) std::cout << 'k';  // bit 3
        if (castling & 4) std::cout << 'q';  // bit 2
    }
    std::cout << '\n';

    std::cout << "En passant: ";
    if (ep_file > 7) std::cout << "-";
    else {
        int rank = (turn == Color::White) ? 5 : 2;  // rank 6 or 3 (0-indexed: 5 or 2)
        std::cout << char('a' + ep_file) << char('1' + rank);
    }
    std::cout << "\n\n";
}

std::string Board::to_fen() const {
    std::string fen;

    // Piece placement
    for (int rank = 7; rank >= 0; --rank) {
        int empty_count = 0;
        for (int file = 0; file < 8; ++file) {
            int sq = rank * 8 + file;
            Piece p = pieces_on_square[sq];
            if (p == Piece::None) {
                empty_count++;
            } else {
                if (empty_count > 0) {
                    fen += char('0' + empty_count);
                    empty_count = 0;
                }
                // Determine color
                bool is_white = occupied[0] & square_bb(sq);
                char c = piece_to_char(p);
                fen += is_white ? c : char(c + 32);  // lowercase for black
            }
        }
        if (empty_count > 0) {
            fen += char('0' + empty_count);
        }
        if (rank > 0) fen += '/';
    }

    // Active color
    fen += ' ';
    fen += (turn == Color::White) ? 'w' : 'b';

    // Castling rights
    fen += ' ';
    if (castling == 0) {
        fen += '-';
    } else {
        if (castling & 2) fen += 'K';
        if (castling & 1) fen += 'Q';
        if (castling & 8) fen += 'k';
        if (castling & 4) fen += 'q';
    }

    // En passant
    fen += ' ';
    if (ep_file > 7) {
        fen += '-';
    } else {
        int ep_rank = (turn == Color::White) ? 5 : 2;
        fen += char('a' + ep_file);
        fen += char('1' + ep_rank);
    }

    // Halfmove clock and fullmove number
    fen += ' ';
    fen += std::to_string(halfmove_clock);
    fen += " 1";  // Fullmove number (we don't track this)

    return fen;
}
