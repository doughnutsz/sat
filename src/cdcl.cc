// Algorithm C from Knuth's The Art of Computer Programming 7.2.2.2: CDCL
//
// This implementation also includes improvements discussed in various 
// exercises, including:
//   - Ex. 257: Redundant literal detection within learned clauses
//   - Ex. 268: Lazy removal of level 0 false lits from clauses
//   - Ex. 270: On-the-fly subsumption
//   - Ex. 271: Subsumption of immediate predecessor learned clauses

#include <ctime>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "counters.h"
#include "flags.h"
#include "heap.h"
#include "logging.h"
#include "timer.h"
#include "types.h"

#define L1(c) (c+1)
#define L0(c) (c)
#define CS(c) (c-1)
#define W0(c) (c-2)
#define W1(c) (c-3)
#define LBD(c) (c-4)

constexpr int kHeaderSize = 4;
constexpr size_t kMaxLemmas = 10000;

enum State {
    UNSET = 0,
    FALSE = 1,           // Trying false, haven't tried true yet.
    TRUE = 2,            // Trying true, haven't tried false yet.
};

// Storage for the DPLL search and the final assignment, if one exists.
struct Cnf {
    std::vector<lit_t> clauses;

    std::vector<State> val;

    std::vector<lit_t> lev;  // maps variable to level it was set on.
    
    std::vector<State> oval;

    std::vector<unsigned long> stamp;  // TODO: what's the right type here?

    std::vector<unsigned long> lstamp;  // maps levels to stamp values

    Heap<2> heap;

    std::vector<lit_t> trail;  // TODO: make sure we're not dynamically resizing during backjump
    // inverse map from literal to trail index. -1 if there's no index in trail.
    std::vector<lit_t> tloc;  // variables -> trail locations; -1 == nil
    size_t f;  // trail length
    size_t g;  // index in trail

    std::vector<size_t> di; // maps d -> last trail position before level d.
    
    std::vector<clause_t> reason;  // Keys: variables, values: clause indices

    std::vector<clause_t> watch_storage;
    clause_t* watch; // Keys: litarals, values: clause indices

    std::vector<lit_t> b;  // temp storage for learned clause

    // temp storage for literal block distance analysis. to compute lbd of a
    // learned clause, we stamp lbds[level(v)] = epoch for each var in the
    // clause and then count how many items in lbds are stamped with epoch.
    std::vector<unsigned long> lbds;
    
    clause_t nclauses;

    lit_t nvars;

    // TODO: explain epoch values here, why they're bumped by 3 each time.
    unsigned long epoch;

    uint32_t agility;

    size_t nlemmas;
    
    Cnf(lit_t nvars, clause_t nclauses) :
        val(nvars + 1, UNSET),
        lev(nvars + 1, -1),
        oval(nvars + 1, FALSE),
        stamp(nvars + 1, 0),
        lstamp(nvars + 1, 0),
        heap(nvars),
        trail(nvars, -1),
        tloc(nvars + 1, -1),
        f(0),
        g(0),
        di(nvars, 0),
        reason(nvars + 1, clause_nil),
        watch_storage(2 * nvars + 1, clause_nil),
        watch(&watch_storage[nvars]),
        b(nvars, -1),
        lbds(nvars, 0),
        nclauses(nclauses),
        nvars(nvars),
        epoch(0),
        agility(0),
        nlemmas(0) {
    }

    // Is the literal x currently false?
    inline bool is_false(lit_t x) const {
        State s = val[abs(x)];
        return (x > 0 && s == FALSE) || (x < 0 && s == TRUE);
    }

    // Is the literal x currently true?
    inline bool is_true(lit_t x) const {
        State s = val[abs(x)];
        return (x > 0 && s == TRUE) || (x < 0 && s == FALSE);
    }    

    std::string print_clause(clause_t c) const {
        std::ostringstream oss;
        oss << "(";
        for (int i = 0; i < clauses[c - 1]; ++i) {
            oss << clauses[c + i];
            if (i != clauses[c - 1] - 1) oss << " ";
        }
        oss << ")";
        return oss.str();
    }

    std::string dump_clauses() const {
        std::ostringstream oss;
        lit_t ts = 0;  // tombstone count
        for(clause_t i = 2; i < clauses.size();
            i += clauses[i] + ts + kHeaderSize + (clauses[i] == 1 ? 1 : 0)) {
            ts = 0;
            oss << "(";
            for(lit_t j = 1; j < clauses[i]; ++j) {
                oss << clauses[i+j] << " ";
            }
            oss << clauses[i+clauses[i]] << ") ";
            while (clauses[i+clauses[i]+1+ts] == lit_nil) ++ts;
        }
        return oss.str();
    }

    std::string raw_clauses() {
        std::ostringstream oss;
        for(const auto& c : clauses) {
            oss << "[" << c << "]";
        }
        return oss.str();
    }
    
    std::string print_trail() {
        std::ostringstream oss;
        for (size_t i = 0; i < f; ++i) {
            oss << "[" << trail[i] << ":" << lev[abs(trail[i])] << "]";
        }
        return oss.str();
    }

    std::string print_watchlist(lit_t l) {
        std::ostringstream oss;
        for (clause_t c = watch[l]; c != clause_nil;
             clauses[c] == l ? (c = clauses[c-2]) : (c = clauses[c-3])) {
            oss << "[" << c << "] " << print_clause(c) << " ";
        }
        return oss.str();
    }

    std::string clause_stats(size_t numb, size_t maxb) const {
        std::ostringstream oss;
        std::vector<int> hist(numb, 0);
        size_t total = 0;
        size_t bucket_size = maxb / numb;
        for(clause_t i = 2; i < clauses.size(); 
            i += clauses[i] + kHeaderSize + (clauses[i] == 1 ? 1 : 0)) {
            size_t v = static_cast<size_t>(clauses[i]);
            size_t vt = v > maxb ? maxb : v;
            hist[vt / bucket_size] += 1;
            total++;
        }
        oss << "(" << total << ") ";
        size_t lower = 0;
        for(const auto& b : hist) {
            size_t upper = lower + bucket_size;
            oss << "[" << lower << ", ";
            if (upper == maxb) { oss << "-"; } else { oss << upper; }
            oss << "): " << b << " ";
            lower = upper;
        }
        return oss.str();
    }

    bool redundant(lit_t l) {
        lit_t k = abs(l);
        clause_t r = reason[k];
        if (r == clause_nil) {
            return false;
        }
        for (lit_t i = 0; i < clauses[r-1]; ++i) {
            lit_t a = clauses[r+i];
            if (k == abs(a)) continue;
            if (lev[abs(a)] == 0) continue;
            if (stamp[abs(a)] == epoch + 2) {
                return false;
            }
            if (stamp[abs(a)] < epoch &&
                (lstamp[lev[abs(a)]] < epoch || !redundant(a))) {
                stamp[abs(a)] = epoch + 2;
                return false;
            }
        }
        stamp[abs(l)] = epoch + 1;
        return true;
    }

    // For a clause c = l_0 l_1 ... l_k at index cindex in the clauses array,
    // removes either l_0 (if offset is 0) or l_1 (if offset is 1) from its
    // watchlist. No-op if k == 0.
    void remove_from_watchlist(lit_t cindex, lit_t offset) {
        if (offset == 1 && clauses[cindex-1] == 1) return;
        lit_t l = cindex + offset;
        clause_t* x = &watch[clauses[l]];
        while (*x != static_cast<clause_t>(cindex)) {
            if (clauses[*x] == clauses[l]) {
                x = (clause_t*)(&clauses[*x-2]);
            } else /* clauses[*x+1] == clauses[l] */ {
                x = (clause_t*)(&clauses[*x-3]);
            }
        }
        *x = clauses[*x-(clauses[*x] == clauses[l] ? 2 : 3)];        
    }

    // Adds l to the trail at level d with reason r.
    void add_to_trail(lit_t l, lit_t d, clause_t r) {
        lit_t k = abs(l);
        tloc[k] = f;
        trail[f] = l;
        ++f;
        val[k] = l < 0 ? FALSE : TRUE;
        lev[k] = d;
        reason[k] = r;
        agility -= (agility >> 13);
        if (oval[k] != val[k]) agility += (1 << 19);
        //LOG(1) << "epoch = " << epoch << ", agility@" << f << ": " << agility / pow(2,32);        
    }

    void backjump(lit_t level) {
        while (f > di[level+1]) {
            f--;
            lit_t l = trail[f];
            lit_t k = abs(l);
            oval[k] = val[k];
            val[k] = UNSET;
            reason[k] = clause_nil;
            heap.insert(k);
        }
        g = f;
    }

    void purge_lemmas() {

    }
};

// Parse a DIMACS cnf input file. File starts with zero or more comments
// followed by a line declaring the number of variables and clauses in the file.
// Each subsequent line is the zero-terminated definition of a disjunction.
// Clauses are specified by integers representing literals, starting at 1.
// Negated literals are represented with a leading minus.
//
// Example: The following CNF formula:
//
//   (x_1 OR x_2) AND (x_3) AND (NOT x_2 OR NOT x_3 OR x_4)
//
// Can be represented with the following file:
//
// c Header comment
// p cnf 4 3
// 1 2 0
// 3 0
// -2 -3 4 0
Cnf parse(const char* filename) {
    int nc;
    FILE* f = fopen(filename, "r");
    CHECK(f) << "Failed to open file: " << filename;

    // Read comment lines until we see the problem line.
    long long nvars = 0, nclauses = 0;
    do {
        nc = fscanf(f, " p cnf %lld %lld \n", &nvars, &nclauses);
        if (nc > 0 && nc != EOF) break;
        nc = fscanf(f, "%*s\n");
    } while (nc != 2 && nc != EOF);
    CHECK(nvars >= 0);
    CHECK(nclauses >= 0);
    CHECK_NO_OVERFLOW(lit_t, nvars);
    CHECK_NO_OVERFLOW(clause_t, nclauses);
    
    // Initialize data structures now that we know nvars and nclauses.
    Cnf c(static_cast<lit_t>(nvars), static_cast<clause_t>(nclauses));

    // Read clauses until EOF.
    int lit;
    do {
        bool read_lit = false;
        c.clauses.push_back(0);        // literal block dist. 0 == never purge.
        c.clauses.push_back(lit_nil);  // watch list ptr for clause's second lit
        c.clauses.push_back(lit_nil);  // watch list ptr for clause's first lit
        c.clauses.push_back(lit_nil);  // size of clause -- don't know this yet
        std::size_t start = c.clauses.size();
        while (true) {
            nc = fscanf(f, " %i ", &lit);
            if (nc == EOF || lit == 0) break;
            c.clauses.push_back(lit);
            read_lit = true;
        }
        int cs = c.clauses.size() - start;
        if (cs == 0 && nc != EOF) {
            LOG(2) << "Empty clause in input file, unsatisfiable formula.";
            UNSAT_EXIT;
        } else if (cs == 0 && nc == EOF) {
            // Clean up from (now unnecessary) c.clauses.push_backs above.
            for(int i = 0; i < kHeaderSize; ++i) { c.clauses.pop_back(); }
        } else if (cs == 1) {
            lit_t x = c.clauses[c.clauses.size() - 1];
            LOG(3) << "Found unit clause " << x;
            State s = x < 0 ? FALSE : TRUE;
            if  (c.val[abs(x)] != UNSET && c.val[abs(x)] != s) {
                LOG(2) << "Contradictory unit clauses, unsatisfiable formula.";
                UNSAT_EXIT;
            }
            c.val[abs(x)] = s;
            c.tloc[abs(x)] = c.f;
            c.trail[c.f++] = x;
            c.lev[abs(x)] = 0;
        }
        if (!read_lit) break;
        CHECK(cs > 0);
        // Record the size of the clause in offset -1.
        c.clauses[start - 1] = cs;
        // TODO: do I need to update watch lists for unit clauses? Going
        // ahead and doing so here until I can verify that I don't have to.
        // Update watch list for the first lit in the clause.
        c.clauses[start - 2] = c.watch[c.clauses[start]];
        c.watch[c.clauses[start]] = start;
        // Update watch list for the second lit in the clause, if one exists.
        if (cs > 1) {
            c.clauses[start - 3] = c.watch[c.clauses[start + 1]];
            c.watch[c.clauses[start + 1]] = start;
        }
    } while (nc != EOF);

    if (c.clauses.empty()) {
        LOG(2) << "No clauses, unsatisfiable.";
        UNSAT_EXIT;
    }
    fclose(f);
    return c;
}


// Returns true exactly when a satisfying assignment exists for c.
bool solve(Cnf* c) {
    Timer t;
    lit_t d = 0;
    unsigned long last_restart = 0;
    
    clause_t lc = clause_nil;  // The most recent learned clause
    while (true) {
        // (C2)
        LOG(4) << "C2";

        //LOG(1) << c->clause_stats(8, 16);

        if (c->f == c->g) {
            LOG(4) << "C5";
            // C5
            if (c->f == static_cast<size_t>(c->nvars)) return true;

            if (c->nlemmas > kMaxLemmas) {
                LOG(1) << "Purging lemmas";
                c->purge_lemmas();
            }
            if (c->agility / pow(2,32) < 0.25 &&
                // If needed, flush literals and continue loop, else
                c->epoch - last_restart >= 1000) {
                // TODO: backjump to some level > d, see Knuth.
                LOG(1) << "Restarting at epoch " << c->epoch;
                c->backjump(0);
                d = 0;
                last_restart = c->epoch;
                continue; // -> C2
            }
            
            ++d;
            c->di[d] = c->f;
            
            // C6
            lit_t k = c->heap.delete_max();
            while (c->val[k] != UNSET) { LOG(3) << k << " unset, rolling again"; k = c->heap.delete_max(); }
            CHECK(k != lit_nil) << "Got nil from heap::delete_max in step C6!";
            LOG(3) << "Decided on variable " << k;
            lit_t l = c->oval[k] == FALSE ? -k : k;
            LOG(3) << "Adding " << l << " to the trail.";
            c->add_to_trail(l, d, clause_nil);
        }

        // C3
        LOG(3) << "C3";
        LOG(3) << "Trail: " << c->print_trail();
        //LOG(3) << "Raw: " << c->raw_clauses();
        LOG(4) << "Clauses: " << c->dump_clauses();
        /*
        for (int ii = 1; ii <= c->nvars; ++ii) {
            LOG(3) << ii << "'s watch list: " << c->print_watchlist(ii);
            LOG(3) << -ii << "'s watch list: " << c->print_watchlist(-ii);
            }*/
        lit_t l = c->trail[c->g];
        LOG(3) << "Examining " << -l << "'s watch list";
        ++c->g;
        clause_t w = c->watch[-l];
        clause_t wll = clause_nil;
        bool found_conflict = false;
        while (w != clause_nil) {

            // C4
            LOG(3) << "C4: l = " << l << ", clause = " << c->print_clause(w);
            if (c->clauses[w] != -l) {
                // Make l0 first literal in the clause instead of the second.
                std::swap(c->clauses[w], c->clauses[w+1]);
                std::swap(c->clauses[w-2], c->clauses[w-3]);
            }
            clause_t nw = c->clauses[w-2];
            LOG(3) << "Looking at watched clause " << c->print_clause(w)
                   << " to see if it forces a unit";
            
            bool all_false = true;
            bool tombstones = false;
            if (!c->is_true(c->clauses[w+1])) {
                for(int i = 2; i < c->clauses[w - 1]; ++i) {
                    // If we see a false literal from level zero, go ahead and
                    // and remove it from the clause now by replacing it with a
                    // tombstone (Ex. 268)
                    if (c->is_false(c->clauses[w + i]) &&
                        c->lev[abs(c->clauses[w + i])] == 0) {
                        c->clauses[w + i] = lit_nil;
                        tombstones = true;
                        continue;
                    } else if (!c->is_false(c->clauses[w + i])) {
                        all_false = false;
                        lit_t ln = c->clauses[w + i];
                        LOG(3) << "Resetting " << ln
                               << " as the watched literal in " << c->print_clause(w);
                        // swap ln and l0
                        std::swap(c->clauses[w], c->clauses[w + i]);
                        // move w onto watch list of ln
                        // TODO: clauses and watch are lit_t and clause_t, resp.
                        //       clean up so we can std::swap here.
                        LOG(4) << "Before putting " << c->print_clause(w)
                               << " on " << ln << "'s watch list: "
                               << c->print_watchlist(ln);
                        size_t tmp = c->watch[ln];
                        c->watch[ln] = w;
                        c->clauses[w - 2] = tmp;
                        LOG(3) << ln;
                        LOG(3) << ln << "'s watch list: " << c->print_watchlist(ln);
                        break;
                    }
                }
                // Compact any tombstones we just added to the clause
                if (tombstones) {
                    int j = 2;
                    for(int i = 2; i < c->clauses[w - 1]; ++i) {
                        if (c->clauses[w + i] != lit_nil) {
                            if (i != j) c->clauses[w + j] = c->clauses[w + i];
                            ++j;
                        }
                    }
                    for(int i = j; i < c->clauses[w - 1]; ++i) {
                        c->clauses[w+i] = lit_nil;
                    }
                    if (j < c->clauses[w - 1]) {
                        INC("tombstoned-level-0-lits", c->clauses[w-1] - j);
                        c->clauses[w-1] = j;
                    }
                }
                
                if (all_false) {
                    if (c->is_false(c->clauses[w+1])) {
                        LOG(3) << c->clauses[w]
                               << " false, everything false! (-> C7)";
                        found_conflict = true;
                        break;
                    } else { // l1 is free
                        lit_t l1 = c->clauses[w+1];
                        LOG(3) << "Adding " << l1 << " to the trail, "
                               << "forced by " << c->print_clause(w);
                        c->add_to_trail(l1, d, w);
                    }
                }

            }

            if (all_false) {
                if (wll == clause_nil) {
                    LOG(4) << "Setting watch[" << -l << "] = "
                           << c->print_clause(w);
                    c->watch[-l] = w;
                }
                else {
                    LOG(4) << "Linking " << -l << "'s watchlist: "
                           << c->print_clause(wll) << " -> " << c->print_clause(w);
                    c->clauses[wll-2] = w;
                }
                wll = w;
            }
                
            LOG(3) << "advancing " << w << " -> " << nw << " with wll=" << wll;
            w = nw;  // advance watch list traversal.
            
            if (w == clause_nil) { LOG(3) << "Hit clause_nil in watch list"; }
            else { LOG(3) << "Moving on to " << c->print_clause(w); }
        }

        // Finish surgery on watchlist
        if (wll == clause_nil) {
            LOG(3) << "Final: Setting watch[" << -l << "] = "
                   << ((w == clause_nil) ? "0" : c->print_clause(w));
            c->watch[-l] = w;
        }
        else {
            LOG(3) << "Final: Linking " << -l << "'s watchlist: "
                   << c->print_clause(wll)
                   << " -> " << ((w == clause_nil) ? "0" : c->print_clause(w));
            c->clauses[wll-2] = w;
        }
        
        if (!found_conflict) {
            //LOG(3) << "Emptying " << -l << "'s watch list";
            //c->watch[-l] = clause_nil;
            LOG(3) << "Didn't find conflict, moving on.";
            continue;
        }

        // C7
        LOG(3) << "Found a conflict with d = " << d;
        if (d == 0) return false;
        
        // (*) Not mentioned in Knuth's description, but we need to make sure
        // that the rightmost literal on the trail is the first literal
        // in the clause here. We'll undo this after the first resolution
        // step below, otherwise watchlists get corrupted.
        size_t rl = c->f - 1;
        size_t cs = static_cast<size_t>(c->clauses[w-1]);
        size_t rl_pos = 0;
        for (bool done = false; !done; --rl) {
            for (rl_pos = 0; rl_pos < cs; ++rl_pos) {
                if (abs(c->trail[rl]) == abs(c->clauses[w+rl_pos])) {
                    done = true;
                    std::swap(c->clauses[w], c->clauses[w+rl_pos]);
                    break;
                }
            }
        }

        lit_t dp = 0;
        lit_t q = 0;
        lit_t r = 0;
        c->epoch += 3;
        LOG(3) << "Bumping epoch to " << c->epoch << " at "
               << c->print_clause(w);
        LOG(3) << "Trail is " << c->print_trail();
        c->stamp[abs(c->clauses[w])] = c->epoch;
        c->heap.bump(abs(c->clauses[w]));

        lit_t t = c->tloc[abs(c->clauses[w])];
        LOG(3) << "RESOLVING [A] " << c->print_clause(w);
        for(size_t j = 1; j < static_cast<size_t>(c->clauses[w-1]); ++j) {
            lit_t m = c->clauses[w+j];
            LOG(4) << "tloc[" << abs(m) << "] = " << c->tloc[abs(m)];
            if (c->tloc[abs(m)] >= t) t = c->tloc[abs(m)];
            // TODO: technically don't need this next line, but it's part of
            // the blit subroutine
            if (c->stamp[abs(m)] == c->epoch) continue;
            c->stamp[abs(m)] = c->epoch;
            lit_t p = c->lev[abs(m)];
            LOG(4) << "Heap is: " << c->heap.debug();
            LOG(4) << "bumping " << abs(m);
            if (p > 0) c->heap.bump(abs(m));
            if (p == d) {
                LOG(3) << m << " is at level " << d;
                q++;
            } else {
                LOG(3) << "Adding " << -m << " (level " << p << ") to learned clause.";
                c->b[r] = -m;
                r++;
                dp = std::max(dp, p);
                c->lstamp[p] =
                    (c->lstamp[p] == c->epoch) ? c->epoch + 1 : c->epoch;
            }
        }
        LOG(3) << "swapping back: " << c->print_clause(w);
        std::swap(c->clauses[w], c->clauses[w+rl_pos]);
        LOG(3) << "now: " << c->print_clause(w);
        
        while (q > 0) {
            LOG(3) << "q=" << q << ",t=" << t;
            lit_t l = c->trail[t];
            t--;
            //LOG(3) << "New L_t = " << c->trail[t];
            if (c->stamp[abs(l)] == c->epoch) {
                LOG(3) << "Stamped this epoch: " << l;
                q--;
                clause_t rc = c->reason[abs(l)];
                if (rc != clause_nil) {
                    LOG(3) << "RESOLVING [B] " << c->print_clause(rc);
                    if (c->clauses[rc] != l) {
                        // TODO: don't swap here (or similar swap above) 
                        std::swap(c->clauses[rc], c->clauses[rc+1]);
                        std::swap(c->clauses[rc-2], c->clauses[rc-3]);
                    }                        
                    LOG(3) << "Reason for " << l << ": " << c->print_clause(rc);
                    for (size_t j = 1; j < static_cast<size_t>(c->clauses[rc-1]); ++j) {
                        lit_t m = c->clauses[rc+j];
                        LOG(3) << "considering " << abs(m);
                        if (c->stamp[abs(m)] == c->epoch) continue;
                        c->stamp[abs(m)] = c->epoch;
                        lit_t p = c->lev[abs(m)];
                        if (p > 0) c->heap.bump(abs(m));
                        if (p == d) {
                            q++;
                        } else {
                            LOG(3) << "Adding " << -m << " to learned clause.";
                            c->b[r] = -m;
                            r++;
                            dp = std::max(dp, p);
                            c->lstamp[p] = (c->lstamp[p] == c->epoch) ? 
                                c->epoch + 1 : c->epoch;
                        }
                    }
                    if (q + r + 1 < c->clauses[rc-1] && q > 0) {
                        c->remove_from_watchlist(rc, 0);
                        lit_t li = lit_nil;
                        lit_t len = c->clauses[rc-1];
                        // Avoid j == 1 below because we'd have to do more
                        // watchlist surgery. A lit of level >= d always
                        // exists in l_2 ... l_k since q > 0.
                        for (lit_t j = len - 1; j >= 2; --j) {
                            if (c->lev[abs(c->clauses[rc+j])] >= d) {
                                li = j;
                                break;
                            }
                        }
                        CHECK(li != lit_nil) <<
                            "No level " << d << " lit for subsumption";
                        c->clauses[rc] = c->clauses[rc+li];
                        c->clauses[rc+li] = c->clauses[rc+len-1];
                        c->clauses[rc+len-1] = lit_nil;
                        c->clauses[rc-1]--;
                        c->clauses[rc-2] = c->watch[c->clauses[rc]];
                        c->watch[c->clauses[rc]] = rc;
                        INC("on-the-fly subsumptions");
                    }
                }
            }
        }

        lit_t lp = c->trail[t];
        LOG(4) << "lp = " << lp;
        while (c->stamp[abs(lp)] != c->epoch) { t--; lp = c->trail[t]; }
        
        LOG(4) << "stopping C7 with l'=" << lp;

        // Remove redundant literals from clause
        // TODO: move this down so that we only process learned clause once? But
        // would also have to do subsumption check in single loop...
        lit_t rr = 0;
        for(int i = 0; i < r; ++i) {
            // TODO: do i need to pass -c->b[i] below? don't think negation matters...
            if (c->lstamp[c->lev[abs(c->b[i])]] == c->epoch + 1 &&
                c->redundant(-c->b[i])) {
                continue;
            }
            c->b[rr] = c->b[i];
            ++rr;
        }
        INC("redundant literals", r - rr);
        r = rr;

        // C8: backjump
        c->backjump(dp);
        d = dp;
        LOG(3) << "After backjump, trail is " << c->print_trail();

        // Ex. 271: Does this clause subsume the previous learned clause? If
        // so, we can "just" overwrite it. lc is the most recent learned clause
        // from a previous iteration.
        if (lc != clause_nil) {
            lit_t q = r+1;
            for (int j = c->clauses[lc-1] - 1; q > 0 && j >= q; --j) {
                if (c->clauses[lc + j] == -lp ||
                    (c->stamp[abs(c->clauses[lc + j])] == c->epoch &&
                     c->val[abs(c->clauses[lc + j])] != UNSET &&
                     c->lev[abs(c->clauses[lc + j])] <= dp)) {
                    --q;
                }
            }

            if (q == 0 && c->val[abs(c->clauses[lc])] == UNSET) {
                c->remove_from_watchlist(lc, 0);
                c->remove_from_watchlist(lc, 1);
                c->clauses.resize(lc-3);
                INC("subsumed clauses");
            }
        }
        
        // C9: learn
        c->clauses.push_back(0);  // literal block distance. will fill below.
        c->clauses.push_back(clause_nil); // watch list for l1
        c->clauses.push_back(c->watch[-lp]); // watch list for l0
        c->clauses.push_back(r+1); // size
        LOG(3) << "adding a clause of size " << r+1;
        lc = c->clauses.size();
        c->clauses.push_back(-lp);
        c->watch[-lp] = lc;
        c->clauses.push_back(clause_nil); // to be set in else below
        bool found_watch = false;
        c->lbds[c->lev[abs(lp)]] = c->epoch;
        for (lit_t j = 0; j < r; ++j) {
            c->lbds[c->lev[abs(c->b[j])]] = c->epoch;
            if (found_watch || c->lev[abs(c->b[j])] < dp) {
                c->clauses.push_back(-c->b[j]);
            } else {
                c->clauses[lc+1] = -c->b[j];
                c->clauses[lc-3] = c->watch[-c->b[j]];
                c->watch[-c->b[j]] = lc;
                found_watch = true;
            }
        }
        CHECK(r == 0 || found_watch) << "Didn't find watched lit in new clause";
        CHECK_NO_OVERFLOW(clause_t, c->clauses.size());
        
        int lbd = 1;  // c->lev[abs(lp)] is > d, so we know it's distinct.
        for (lit_t j = 0; j <= d; ++j) { if (c->lbds[j] == c->epoch) ++lbd; }
        c->clauses[LBD(lc)] = lbd;
        if (lbd <= 3) LOG(1) << "lbd: " << lbd << ": " << c->print_clause(lc);

        ++c->nlemmas;
        LOG(2) << "Successfully added clause " << c->print_clause(lc);
        LOG(2) << "trail: " << c->print_trail();
        INC("learned clause literals", r+1);
        INC("learned clauses");

        c->add_to_trail(-lp, d, lc);
        c->heap.rescale_delta();
        
        LOG(3) << "After clause install, trail is " << c->print_trail();
    }

    return true;
}

int main(int argc, char** argv) {
    int oidx;
    CHECK(parse_flags(argc, argv, &oidx)) <<
        "Usage: " << argv[0] << " <filename>";
    init_counters();
    Cnf c = parse(argv[oidx]);
    // TODO: also check for no clauses (unsatisfiable) in the if
    // statement below.
    if (solve(&c)) {
        std::cout << "s SATISFIABLE" << std::endl;
        for (int i = 1, j = 0; i <= c.nvars; ++i) {
            if (c.val[i] == UNSET) continue;
            if (j % 10 == 0) std::cout << "v";
            std::cout << ((c.val[i] & 1) ? " -" : " ") << i;
            ++j;
            if (i == c.nvars) std::cout << " 0" << std::endl;
            else if (j > 0 && j % 10 == 0) std::cout << std::endl;
         }
        return SATISFIABLE;
    } else {
        std::cout << "s UNSATISFIABLE" << std::endl;
        return UNSATISFIABLE;
    }
}
