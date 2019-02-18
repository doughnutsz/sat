// Algorithm C from Knuth's The Art of Computer Programming 7.2.2.2: CDCL

#include <ctime>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "flags.h"
#include "logging.h"
#include "types.h"
#include "heap.h"

enum State {
    UNSET = 0,
    FALSE = 1,           // Trying false, haven't tried true yet.
    TRUE = 2,            // Trying true, haven't tried false yet.
};

// Storage for the DPLL search and the final assignment, if one exists.
struct Cnf {
    std::vector<lit_t> clauses;

    std::vector<State> val;

    std::vector<State> oval;

    std::vector<unsigned long> stamp;  // TODO: what's the right type here?

    Heap<2> heap;

    std::vector<lit_t> trail;
    // inverse map from literal to trail index. -1 if there's no index in trail.
    std::vector<size_t> tloc;  // -1 == nil
    size_t g;  // index in trail

    std::vector<clause_t> reason_storage;
    clause_t* reason; // Keys: literals, values: clause indices

    std::vector<clause_t> watch_storage;
    clause_t* watch; // Keys: litarals, values: clause indices

    clause_t maxl;

    clause_t minl;

    clause_t nclauses;

    lit_t nvars;

    lit_t f;

    Cnf(lit_t nvars, clause_t nclauses) :
        val(nvars + 1, UNSET),
        oval(nvars + 1, FALSE),
        stamp(nvars + 1, 0),
        heap(nvars),
        tloc(nvars + 1, -1),
        reason_storage(2 * nvars + 1, clause_nil),
        reason(&reason_storage[nvars]),
        watch_storage(2 * nvars + 1, clause_nil),
        watch(&watch_storage[nvars]),
        nclauses(nclauses),
        nvars(nvars),
        f(0) {
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

    std::string print_clause(clause_t c) {
        std::ostringstream oss;
        for (int i = 0; i < clauses[c - 1]; ++i) {
            oss << clauses[c + i];
            if (i != clauses[c - 1] - 1) oss << " ";
        }
        return oss.str();
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
    assert(nvars >= 0);
    assert(nclauses >= 0);
    ASSERT_NO_OVERFLOW(lit_t, nvars);
    ASSERT_NO_OVERFLOW(clause_t, nclauses);

    // Initialize data structures now that we know nvars and nclauses.
    Cnf c(static_cast<lit_t>(nvars), static_cast<clause_t>(nclauses));

    // Read clauses until EOF.
    int lit;
    do {
        bool read_lit = false;
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
        } else if (cs == 1) {
            lit_t x = c.clauses[c.clauses.size() - 1];
            LOG(3) << "Found unit clause " << x;
            State s = x < 0 ? FALSE : TRUE;
            if  (c.val[abs(x)] != UNSET && c.val[abs(x)] != s) {
                LOG(2) << "Contradictory unit clauses, unsatisfiable formula.";
                UNSAT_EXIT;
            }
            c.val[abs(x)] = s;
            c.tloc[abs(x)] = c.trail.size();
            c.trail.push_back(x);
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

    c.minl = c.maxl = c.clauses.size() + 1;
    fclose(f);
    return c;
}


// Returns true exactly when a satisfying assignment exists for c.
bool solve(Cnf* c) {
    lit_t d = 0;
    size_t g = 0;
    while (c->trail.size() < static_cast<size_t>(c->nvars)) {
        // (C2)
        while (c->trail.size() == g) {
            // C5
            if (c->trail.size() == static_cast<size_t>(c->nvars)) return true;
            // TODO: If needed, purge excess clauses, else
            // TODO: If needed, flush literals and continue loop, else
            ++d;
            // i_d = c->trail.size() ??

            // C6
            lit_t k = c->heap.delete_max();
            while (c->val[k] != UNSET) k = c->heap.delete_max();
            LOG(3) << "Decided on variable " << k;
            lit_t l = c->oval[k] == FALSE ? -k : k;
            LOG(3) << "Adding " << l << " to the trail.";
            c->tloc[k] = c->trail.size();
            c->trail.push_back(l);
            c->val[k] = l < 0 ? FALSE : TRUE;
            c->reason[l] = clause_nil;
            break;
        }

        // C3
        lit_t l = c->trail[g];
        LOG(3) << "Examining " << -l << "'s watch list";
        ++g;
        clause_t w = c->watch[-l];
        bool found_conflict = false;
        while (w != clause_nil) {

            // C4
            size_t l0_i = c->clauses[w] == -l ? w+1 : w;
            lit_t l0 = c->clauses[l0_i];
            LOG(3) << "Looking at watched clause " << c->print_clause(w)
                   << " to see if it forces a unit (" << l0 << ")";
            if (!c->is_true(l0)) {
                bool all_false = true;
                for(int i = 2; i < c->clauses[w - 1]; ++i) {
                    if (!c->is_false(c->clauses[w + i])) {
                        all_false = false;
                        lit_t ln = c->clauses[w + i];
                        LOG(3) << "Resetting " << ln
                               << " as the watched literal in " << c->print_clause(w);
                        // swap ln and l0
                        std::swap(c->clauses[l0_i], c->clauses[w + i]);
                        // move w onto watch list of ln
                        // TODO: clauses and watch are lit_t and clause_t, resp.
                        //       clean up so we can std::swap here.
                        size_t wl_i = c->clauses[w] == -l ? w - 2 : w - 3;
                        size_t tmp = c->watch[ln];
                        c->watch[ln] = c->clauses[wl_i];
                        c->clauses[wl_i] = tmp;
                        break;
                    }
                }
                if (all_false) {
                    if (c->is_false(l0)) {
                        LOG(3) << l0 << " is false, everything false! (-> C7)";
                        break;
                    } else { // l0 is free
                        LOG(3) << "Adding " << l0 << " to the trail as "
                               << (l0 < 0 ? FALSE : TRUE);
                        c->tloc[abs(l0)] = c->trail.size();
                        c->trail.push_back(l0);
                        c->val[abs(l0)] = l0 < 0 ? FALSE : TRUE;
                        c->reason[l] = w;
                    }
                }
            }
            w = c->clauses[w] == -l ? c->clauses[w - 2] : c->clauses[w - 3];
        }

        if (!found_conflict) {
            LOG(3) << "Emptying " << -l << "'s watch list";
            c->watch[-l] = clause_nil;
        } else {
            // C7
        }
    }

    return true;
}

int main(int argc, char** argv) {
    int oidx;
    CHECK(parse_flags(argc, argv, &oidx)) <<
        "Usage: " << argv[0] << " <filename>";
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
