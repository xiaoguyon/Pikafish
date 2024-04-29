/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2024 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "uci.h"

#include <stdint.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#include "benchmark.h"
#include "engine.h"
#include "evaluate.h"
#include "movegen.h"
#include "position.h"
#include "score.h"
#include "search.h"
#include "types.h"
#include "ucioption.h"
#include "perft.h"

#if defined(__EMSCRIPTEN__)
    #define EM_STATIC static
#else
    #define EM_STATIC 
#endif

namespace Stockfish {

constexpr auto StartFEN  = "rnbakabnr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/1C5C1/9/RNBAKABNR w";
constexpr int  MaxHashMB = Is64Bit ? 33554432 : 2048;

template<typename... Ts>
struct overload: Ts... {
    using Ts::operator()...;
};

template<typename... Ts>
overload(Ts...) -> overload<Ts...>;

UCIEngine::UCIEngine(int argc, char** argv) :
    engine(argv[0]),
    cli(argc, argv) {

    auto& options = engine.get_options();

    options["Debug Log File"] << Option("", [](const Option& o) { start_logger(o); });

    options["Threads"] << Option(1, 1, 1024, [this](const Option&) { engine.resize_threads(); });

    options["Hash"] << Option(16, 1, MaxHashMB, [this](const Option& o) { engine.set_tt_size(o); });

    options["Clear Hash"] << Option([this](const Option&) { engine.search_clear(); });
    options["Ponder"] << Option(false);
    options["MultiPV"] << Option(1, 1, MAX_MOVES);
    options["Move Overhead"] << Option(10, 0, 5000);
    options["nodestime"] << Option(0, 0, 10000);
    options["UCI_ShowWDL"] << Option(false);
    options["EvalFile"] << Option(EvalFileDefaultName,
                                  [this](const Option& o) { engine.load_network(o); });


    engine.set_on_iter([](const auto& i) { on_iter(i); });
    engine.set_on_update_no_moves([](const auto& i) { on_update_no_moves(i); });
    engine.set_on_update_full([&](const auto& i) { on_update_full(i, options["UCI_ShowWDL"]); });
    engine.set_on_bestmove([](const auto& bm, const auto& p) { on_bestmove(bm, p); });

    engine.load_network(options["EvalFile"]);
    engine.resize_threads();
    engine.search_clear();  // After threads are up
}

void UCIEngine::loop() {

    EM_STATIC Position     pos;
    EM_STATIC StateListPtr states(new std::deque<StateInfo>(1));
    [[maybe_unused]] EM_STATIC auto __init_once = [&]() {
        pos.set(StartFEN, &states->back());
        return 0;
    }();

    for (int i = 1; i < cli.argc; ++i)
        cmd += std::string(cli.argv[i]) + " ";

    do
    {
        if (cli.argc == 1
            && !getline(std::cin, cmd))  // Wait for an input or an end-of-file (EOF) indication
            cmd = "quit";


        std::istringstream is(cmd);

        token.clear();  // Avoid a stale if getline() returns nothing or a blank line
        is >> std::skipws >> token;

        if (token == "quit" || token == "stop")
            engine.stop();

        // The GUI sends 'ponderhit' to tell that the user has played the expected move.
        // So, 'ponderhit' is sent if pondering was done on the same move that the user
        // has played. The search should continue, but should also switch from pondering
        // to the normal search.
        else if (token == "ponderhit")
            engine.set_ponderhit(false);

        else if (token == "uci")
            sync_cout << "id name " << engine_info(true) << "\n"
                      << engine.get_options() << "\nuciok" << sync_endl;

        else if (token == "setoption")
            setoption(is);
        else if (token == "go")
            go(is);
        else if (token == "position")
            position(is);
        else if (token == "fen" || token == "startpos")
            is.seekg(0), position(is);
        else if (token == "ucinewgame")
            engine.search_clear();
        else if (token == "isready")
            sync_cout << "readyok" << sync_endl;

        // Add custom non-UCI commands, mainly for debugging purposes.
        // These commands must not be used during a search!
        else if (token == "flip")
            engine.flip();
        else if (token == "bench")
            bench(is);
        else if (token == "d")
            sync_cout << engine.visualize() << sync_endl;
        else if (token == "eval")
            engine.trace_eval();
        else if (token == "compiler")
            sync_cout << compiler_info() << sync_endl;
        else if (token == "export_net")
        {
            std::optional<std::string> file;
            std::string                f;
            if (is >> std::skipws >> f)
                file = f;
            engine.save_network(file);
        }
        else if (token == "--help" || token == "help" || token == "--license" || token == "license")
            sync_cout
              << "\nPikafish is a powerful xiangqi engine for playing and analyzing."
                 "\nIt is released as free software licensed under the GNU GPLv3 License."
                 "\nPikafish is normally used with a graphical user interface (GUI) and implements"
                 "\nthe Universal Chess Interface (UCI) protocol to communicate with a GUI, an API, etc."
                 "\nFor any further information, visit https://github.com/official-pikafish/Pikafish#readme"
                 "\nor read the corresponding README.md and Copying.txt files distributed along with this program.\n"
              << sync_endl;
        else if (!token.empty() && token[0] != '#')
            sync_cout << "Unknown command: '" << cmd << "'. Type help for more information."
                      << sync_endl;

    } while (token != "quit" && cli.argc == 1);  // The command-line arguments are one-shot
}

Search::LimitsType UCIEngine::parse_limits(std::istream& is) {
    Search::LimitsType limits;
    std::string        token;

    limits.startTime = now();  // The search starts as early as possible

    while (is >> token)
        if (token == "searchmoves")  // Needs to be the last command on the line
            while (is >> token)
                limits.searchmoves.push_back(token);

        else if (token == "wtime")
            is >> limits.time[WHITE];
        else if (token == "btime")
            is >> limits.time[BLACK];
        else if (token == "winc")
            is >> limits.inc[WHITE];
        else if (token == "binc")
            is >> limits.inc[BLACK];
        else if (token == "movestogo")
            is >> limits.movestogo;
        else if (token == "depth")
            is >> limits.depth;
        else if (token == "nodes")
            is >> limits.nodes;
        else if (token == "movetime")
            is >> limits.movetime;
        else if (token == "mate")
            is >> limits.mate;
        else if (token == "perft")
            is >> limits.perft;
        else if (token == "infinite")
            limits.infinite = 1;
        else if (token == "ponder")
            limits.ponderMode = true;

    return limits;
}

void UCIEngine::go(std::istringstream& is) {

    Search::LimitsType limits = parse_limits(is);

    if (limits.perft)
        perft(limits);
    else
        engine.go(limits);
}

void UCIEngine::bench(std::istream& args) {
    std::string token;
    uint64_t    num, nodes = 0, cnt = 1;
    uint64_t    nodesSearched = 0;
    const auto& options       = engine.get_options();

    engine.set_on_update_full([&](const auto& i) {
        nodesSearched = i.nodes;
        on_update_full(i, options["UCI_ShowWDL"]);
    });

    std::vector<std::string> list = Benchmark::setup_bench(engine.fen(), args);

    num = count_if(list.begin(), list.end(),
                   [](const std::string& s) { return s.find("go ") == 0 || s.find("eval") == 0; });

    TimePoint elapsed = now();

    for (const auto& cmd : list)
    {
        std::istringstream is(cmd);
        is >> std::skipws >> token;

        if (token == "go" || token == "eval")
        {
            std::cerr << "\nPosition: " << cnt++ << '/' << num << " (" << engine.fen() << ")"
                      << std::endl;
            if (token == "go")
            {
                Search::LimitsType limits = parse_limits(is);

                if (limits.perft)
                    nodes = perft(limits);
                else
                {
                    engine.go(limits);
                    engine.wait_for_search_finished();
                }

                nodes += nodesSearched;
                nodesSearched = 0;
            }
            else
                engine.trace_eval();
        }
        else if (token == "setoption")
            setoption(is);
        else if (token == "position")
            position(is);
        else if (token == "ucinewgame")
        {
            engine.search_clear();  // search_clear may take a while
            elapsed = now();
        }
    }

    elapsed = now() - elapsed + 1;  // Ensure positivity to avoid a 'divide by zero'

    dbg_print();

    std::cerr << "\n==========================="
              << "\nTotal time (ms) : " << elapsed << "\nNodes searched  : " << nodes
              << "\nNodes/second    : " << 1000 * nodes / elapsed << std::endl;

    // reset callback, to not capture a dangling reference to nodesSearched
    engine.set_on_update_full([&](const auto& i) { on_update_full(i, options["UCI_ShowWDL"]); });
}


void UCIEngine::setoption(std::istringstream& is) {
    engine.wait_for_search_finished();
    engine.get_options().setoption(is);
}

std::uint64_t UCIEngine::perft(const Search::LimitsType& limits) {
    auto nodes = engine.perft(engine.fen(), limits.perft);
    sync_cout << "\nNodes searched: " << nodes << "\n" << sync_endl;
    return nodes;
}

void UCIEngine::position(std::istringstream& is) {
    std::string token, fen;

    is >> token;

    if (token == "startpos")
    {
        fen = StartFEN;
        is >> token;  // Consume the "moves" token, if any
    }
    else if (token == "fen")
        while (is >> token && token != "moves")
            fen += token + " ";
    else
        return;

    std::vector<std::string> moves;

    while (is >> token)
    {
        moves.push_back(token);
    }

    engine.set_position(fen, moves);
}

namespace {

struct WinRateParams {
    double a;
    double b;
};

WinRateParams win_rate_params(const Position& pos) {

    int material = 10 * pos.count<ROOK>() + 5 * pos.count<KNIGHT>() + 5 * pos.count<CANNON>()
                 + 3 * pos.count<BISHOP>() + 2 * pos.count<ADVISOR>() + pos.count<PAWN>();

    // The fitted model only uses data for material counts in [10, 110], and is anchored at count 53.
    double m = std::clamp(material, 10, 110) / 53.0;

    // Return a = p_a(material) and b = p_b(material), see github.com/official-stockfish/WDL_model
    constexpr double as[] = {229.68413041, -836.53336539, 1004.77236193, 18.19226434};
    constexpr double bs[] = {114.18428891, -392.54680852, 475.32622987, -123.49708474};

    double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

    return {a, b};
}

// The win rate model is 1 / (1 + exp((a - eval) / b)), where a = p_a(material) and b = p_b(material).
// It fits the LTC fishtest statistics rather accurately.
int win_rate_model(Value v, const Position& pos) {

    auto [a, b] = win_rate_params(pos);

    // Return the win rate in per mille units, rounded to the nearest integer.
    return int(0.5 + 1000 / (1 + std::exp((a - double(v)) / b)));
}
}

std::string UCIEngine::format_score(const Score& s) {
    const auto format = overload{[](Score::Mate mate) -> std::string {
                                     auto m = (mate.plies > 0 ? (mate.plies + 1) : mate.plies) / 2;
                                     return std::string("mate ") + std::to_string(m);
                                 },
                                 [](Score::InternalUnits units) -> std::string {
                                     return std::string("cp ") + std::to_string(units.value);
                                 }};

    return s.visit(format);
}

// Turns a Value to an integer centipawn number,
// without treatment of mate and similar special scores.
int UCIEngine::to_cp(Value v, const Position& pos) {

    // In general, the score can be defined via the the WDL as
    // (log(1/L - 1) - log(1/W - 1)) / ((log(1/L - 1) + log(1/W - 1))
    // Based on our win_rate_model, this simply yields v / a.

    auto [a, b] = win_rate_params(pos);

    return std::round(100 * int(v) / a);
}

std::string UCIEngine::wdl(Value v, const Position& pos) {
    std::stringstream ss;

    int wdl_w = win_rate_model(v, pos);
    int wdl_l = win_rate_model(-v, pos);
    int wdl_d = 1000 - wdl_w - wdl_l;
    ss << " wdl " << wdl_w << " " << wdl_d << " " << wdl_l;

    return ss.str();
}

std::string UCIEngine::square(Square s) {
    return std::string{char('a' + file_of(s)), char('0' + rank_of(s))};
}

std::string UCIEngine::move(Move m) {
    if (m == Move::none())
        return "(none)";

    if (m == Move::null())
        return "0000";

    Square from = m.from_sq();
    Square to   = m.to_sq();

    std::string move = square(from) + square(to);

    return move;
}

Move UCIEngine::to_move(const Position& pos, std::string str) {
    for (const auto& m : MoveList<LEGAL>(pos))
        if (str == move(m))
            return m;

    return Move::none();
}

void UCIEngine::on_update_no_moves(const Engine::InfoShort& info) {
    sync_cout << "info depth " << info.depth << " score " << format_score(info.score) << sync_endl;
}

void UCIEngine::on_update_full(const Engine::InfoFull& info, bool showWDL) {
    std::stringstream ss;

    ss << "info";
    ss << " depth " << info.depth                 //
       << " seldepth " << info.selDepth           //
       << " multipv " << info.multiPV             //
       << " score " << format_score(info.score);  //

    if (showWDL)
        ss << " wdl " << info.wdl;

    if (!info.bound.empty())
        ss << " " << info.bound;

    ss << " nodes " << info.nodes        //
       << " nps " << info.nps            //
       << " hashfull " << info.hashfull  //
       << " tbhits " << info.tbHits      //
       << " time " << info.timeMs        //
       << " pv " << info.pv;             //

    sync_cout << ss.str() << sync_endl;
}

void UCIEngine::on_iter(const Engine::InfoIter& info) {
    std::stringstream ss;

    ss << "info";
    ss << " depth " << info.depth                     //
       << " currmove " << info.currmove               //
       << " currmovenumber " << info.currmovenumber;  //

    sync_cout << ss.str() << sync_endl;
}

void UCIEngine::on_bestmove(std::string_view bestmove, std::string_view ponder) {
    sync_cout << "bestmove " << bestmove;
    if (!ponder.empty())
        std::cout << " ponder " << ponder;
    std::cout << sync_endl;
}

}  // namespace Stockfish

// Execute UCI::loop() only once.
extern "C" void wasm_uci_execute() {
    using namespace Stockfish;

    std::string input;
    std::getline(std::cin, input);
    char *argv[2] = {input.data(), input.data()};
    CommandLine cli(2, argv);

    EM_STATIC std::unique_ptr<UCI> uci;
    [[maybe_unused]] EM_STATIC auto __init_once = [&]() {
        Bitboards::init();
        Position::init();
        uci = std::make_unique<UCI>(2, argv);
        Tune::init(uci->options);
        uci->evalFile = Eval::NNUE::load_networks(uci->workingDirectory(), uci->options, uci->evalFile);
        return 0;
    }();

    uci->set_cli(cli);
    uci->loop();
}
