#include "graph.hpp"
#include "error.hpp"

#include <cassert>
#include <fstream>
#include <map>
#include <numeric>
#include <queue>
#include <regex>
#include <set>
#include <sstream>

#include <spdlog/spdlog.h>

Graph::Graph()
{
}

Graph::Graph(State init_st, const std::set<State> &final_sts,
             const DFADelta &delta)
    : table_(),
      states_at_depth_(),
      final_state_(final_sts),
      init_state_(init_st)
{
    for (auto &&[q, q0, q1] : delta)
        table_.push_back(TableItem{q, q0, q1, {}, {}});
    for (auto &&item : table_) {
        table_.at(item.child0).parents0.push_back(item.index);
        table_.at(item.child1).parents1.push_back(item.index);
    }
}

Graph Graph::from_file(const std::string &filename)
{
    std::string field;
    auto load_comma_separated_states =
        [](const std::string &s) -> std::vector<State> {
        if (s == "_")
            return {};

        // Thanks to: https://faithandbrave.hateblo.jp/entry/2014/05/01/171631
        std::vector<State> ret;
        std::istringstream iss{s};
        std::string field;
        try {
            while (std::getline(iss, field, ','))
                ret.push_back(std::stoi(field));
        }
        catch (std::invalid_argument e) {
            error::die(
                "Expected number as comma separted state, but got {} ({})",
                field, e.what());
        }
        return ret;
    };

    std::set<State> init_sts, final_sts;
    NFADelta delta;
    std::regex re(R"(^(>)?(\d+)(\*)?\s+(_|[\d,]+)\s+(_|[\d,]+)$)");
    std::smatch match;
    bool is_dfa = true;
    std::ifstream ifs{filename};
    assert(ifs);
    std::string line;
    while (std::getline(ifs, line)) {
        if (!std::regex_match(line, match, re)) {
            spdlog::info("Skip line \"{}\"", line);
            continue;
        }

        bool initial = match[1].matched, final = match[3].matched;
        State q = delta.size();
        std::vector<State> q0s = load_comma_separated_states(match[4].str()),
                           q1s = load_comma_separated_states(match[5].str());

        // validate
        if (q != std::stoi(match[2].str()))
            error::die("Invalid state number: {} != {}",
                       std::stoi(match[2].str()), q);

        if (initial)
            init_sts.insert(q);
        if (final)
            final_sts.insert(q);
        delta.emplace_back(q, q0s, q1s);

        if (q0s.size() != 1 || q1s.size() != 1)
            is_dfa = false;
    }
    if (init_sts.size() != 1)
        is_dfa = false;

    if (is_dfa) {
        assert(init_sts.size() == 1);
        DFADelta new_delta;
        for (auto &&[q, q0s, q1s] : delta) {
            assert(q0s.size() == 1 && q1s.size() == 1);
            new_delta.emplace_back(q, q0s.at(0), q1s.at(0));
        }
        return Graph{*init_sts.begin(), final_sts, new_delta};
    }

    return Graph::from_nfa(init_sts, final_sts, delta);
}

// Input  NFA Mn: (Qn, {0, 1}, dn, q0n, Fn)
// Output DFA Md: (Qd, {0, 1}, df, q0f, Ff)
Graph Graph::from_nfa(const std::set<State> &q0n, const std::set<State> &Fn,
                      const NFADelta &dn)
{
    using StateSubset = std::set<State>;

    // 2^{Qn} -> Qd
    std::map<StateSubset, State> st_map;
    // df
    DFADelta df;
    // Ff
    std::set<State> Ff;

    auto get_or_create_state = [&](const StateSubset &qs) {
        auto it = st_map.find(qs);
        if (it != st_map.end())
            return it->second;
        State qsd = df.size();
        df.emplace_back(qsd, -1, -1);
        st_map.emplace(qs, qsd);
        return qsd;
    };

    /*
    auto dump_qs = [](const auto &qs, std::string prefix = "") {
        std::stringstream ss;
        ss << prefix << "[";
        for (State q : qs)
            ss << q << " ";
        ss << "]";
        spdlog::info("{}", ss.str());
    };
    */

    std::set<StateSubset> visited;
    std::queue<StateSubset> que;
    que.push(q0n);
    while (!que.empty()) {
        StateSubset qs = que.front();
        que.pop();
        if (visited.contains(qs))
            continue;

        StateSubset qs0, qs1;
        for (State q : qs) {
            auto &&[q_, dst0, dst1] = dn.at(q);
            qs0.insert(dst0.begin(), dst0.end());
            qs1.insert(dst1.begin(), dst1.end());
        }
        bool final = std::any_of(qs.begin(), qs.end(),
                                 [&Fn](size_t q) { return Fn.contains(q); });

        State qsd = get_or_create_state(qs), qs0d = get_or_create_state(qs0),
              qs1d = get_or_create_state(qs1);
        df.at(qsd) = std::make_tuple(qsd, qs0d, qs1d);

        visited.insert(qs);
        if (final)
            Ff.insert(qsd);
        que.push(qs0);
        que.push(qs1);
    }

    return Graph{get_or_create_state(q0n), Ff, df};
}

size_t Graph::size() const
{
    return table_.size();
}

bool Graph::is_final_state(State state) const
{
    return final_state_.contains(state);
}

Graph::State Graph::next_state(State state, bool input) const
{
    auto &t = table_.at(state);
    return input ? t.child1 : t.child0;
}

std::vector<Graph::State> Graph::prev_states(State state, bool input) const
{
    auto &t = table_.at(state);
    return input ? t.parents1 : t.parents0;
}

Graph::State Graph::initial_state() const
{
    return init_state_;
}

void Graph::reserve_states_at_depth(size_t depth)
{
    states_at_depth_.clear();
    states_at_depth_.shrink_to_fit();
    states_at_depth_.reserve(depth);

    std::vector<bool> sts(size(), false);
    sts.at(initial_state()) = true;
    std::vector<State> tmp;
    tmp.reserve(size());
    for (size_t i = 0; i < depth; i++) {
        for (Graph::State st = 0; st < size(); st++) {
            if (sts.at(st))
                tmp.push_back(st);
            sts.at(st) = false;
        }
        states_at_depth_.push_back(tmp);

        for (State st : tmp) {
            sts.at(next_state(st, false)) = true;
            sts.at(next_state(st, true)) = true;
        }

        tmp.clear();
    }
}

std::vector<Graph::State> Graph::states_at_depth(size_t depth) const
{
    return states_at_depth_.at(depth);
}

std::vector<Graph::State> Graph::all_states() const
{
    std::vector<State> ret(size());
    std::iota(ret.begin(), ret.end(), 0);
    return ret;
}

Graph Graph::reversed() const
{
    NFADelta delta(size());
    for (State q : all_states()) {
        delta.at(q) =
            std::make_tuple(q, prev_states(q, false), prev_states(q, true));
    }
    return Graph::from_nfa(final_state_, {initial_state()}, delta);
}

Graph Graph::minimized() const
{
    return removed_unreachable().grouped_nondistinguishable();
}

Graph Graph::removed_unreachable() const
{
    std::set<State> reachable;
    std::queue<State> que;
    que.push(initial_state());
    while (!que.empty()) {
        State q = que.front();
        que.pop();
        reachable.insert(q);
        State q0 = next_state(q, false), q1 = next_state(q, true);
        if (!reachable.contains(q0))
            que.push(q0);
        if (!reachable.contains(q1))
            que.push(q1);
    }

    std::unordered_map<State, State> old2new;
    for (State old : reachable)
        old2new.emplace(old, old2new.size());

    State init_st = old2new.at(initial_state());
    std::set<State> final_sts;
    for (State q : final_state_)
        if (reachable.contains(q))
            final_sts.insert(old2new.at(q));
    DFADelta delta(reachable.size());
    for (auto &&entry : table_) {
        if (!reachable.contains(entry.index))
            continue;
        State q = old2new.at(entry.index), q0 = old2new.at(entry.child0),
              q1 = old2new.at(entry.child1);
        delta.at(q) = std::make_tuple(q, q0, q1);
    }

    return Graph{init_st, final_sts, delta};
}

Graph Graph::grouped_nondistinguishable() const
{
    // Table-filling algorithm
    std::vector<bool> table(size() * size(), false);  // FIXME: use only half
    std::queue<std::pair<State, State>> que;
    for (State qa = 0; qa < size(); qa++) {
        for (State qb = qa + 1; qb < size(); qb++) {
            bool qa_final = is_final_state(qa), qb_final = is_final_state(qb);
            if (qa_final && !qb_final || !qa_final && qb_final) {
                que.push(std::make_pair(qa, qb));
                table.at(qa + qb * size()) = true;
            }
        }
    }
    while (!que.empty()) {
        auto &&[ql, qr] = que.front();
        que.pop();
        assert(ql < qr);
        for (bool in : std::vector{true, false}) {
            for (State qa : prev_states(ql, in)) {
                for (State qb : prev_states(qr, in)) {
                    if (qa < qb && !table.at(qa + size() * qb)) {
                        table.at(qa + size() * qb) = true;
                        que.push(std::make_pair(qa, qb));
                    }
                    else if (qa > qb && !table.at(qb + size() * qa)) {
                        table.at(qb + size() * qa) = true;
                        que.push(std::make_pair(qb, qa));
                    }
                }
            }
        }
    }

    // Group the states
    // FIXME: must find more efficient way. maybe union-find?
    std::vector<std::set<State>> st;
    auto find_contains = [&st](State q) {
        return std::find_if(st.begin(), st.end(),
                            [q](auto &s) { return s.contains(q); });
    };
    for (State q : all_states())
        st.push_back({q});
    for (State qa = 0; qa < size(); qa++) {
        for (State qb = qa + 1; qb < size(); qb++) {
            bool equiv = !table.at(qa + qb * size());
            if (!equiv)
                continue;
            auto ita = find_contains(qa), itb = find_contains(qb);
            assert(ita != st.end() && itb != st.end());
            if (ita == itb)
                continue;
            ita->insert(itb->begin(), itb->end());
            st.erase(itb);
        }
    }

    State init_st;
    std::set<State> final_sts;
    DFADelta delta;
    for (size_t q = 0; q < st.size(); q++) {
        std::set<State> &s = st.at(q);
        State repr = *s.begin();

        bool initial = std::any_of(s.begin(), s.end(), [this](State q) {
            return initial_state() == q;
        });
        bool final = is_final_state(repr);
        if (initial)
            init_st = q;
        if (final)
            final_sts.insert(q);

        auto q0_it = find_contains(next_state(repr, false)),
             q1_it = find_contains(next_state(repr, true));
        assert(q0_it != st.end() && q1_it != st.end());
        State q0 = q0_it - st.begin(), q1 = q1_it - st.begin();

        delta.emplace_back(q, q0, q1);
    }

    return Graph{init_st, final_sts, delta};
}

void Graph::dump(std::ostream &os) const
{
    for (Graph::State q : all_states()) {
        if (initial_state() == q)
            os << ">";
        os << q;
        if (is_final_state(q))
            os << "*";
        os << "\t";
        os << next_state(q, false);
        os << "\t";
        os << next_state(q, true);
        os << "\n";
    }
}

Graph::NFADelta reversed_nfa_delta(const Graph::NFADelta &src)
{
    std::vector<std::vector<Graph::State>> prev0(src.size()), prev1(src.size());
    for (Graph::State q = 0; q < src.size(); q++) {
        auto &&[q_, q0s, q1s] = src.at(q);
        for (Graph::State q0 : q0s)
            prev0.at(q0).push_back(q);
        for (Graph::State q1 : q1s)
            prev1.at(q1).push_back(q);
    }

    Graph::NFADelta ret(src.size());
    for (Graph::State q = 0; q < src.size(); q++)
        ret.at(q) = std::make_tuple(q, prev0.at(q), prev1.at(q));
    return ret;
}
