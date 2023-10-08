#include "internal.hpp"

namespace CaDiCaL {

// specific warmup version with saving of the target.
inline void Internal::warmup_assign (int lit, Clause *reason) {

  if (level)
    require_mode (SEARCH);

  const int idx = vidx (lit);
  assert (reason != external_reason);
  const bool from_external = false;
  assert (reason != external_reason);
  assert (!vals[idx]);
  assert (!flags (idx).eliminated ());
  Var &v = var (idx);
  int lit_level;
  assert (!(reason == external_reason &&
	    ((size_t) level <= assumptions.size () + (!!constraint.size ()))));
  assert (reason);
  assert (level);
  // we  purely assign in order here
  lit_level = level;

  v.level = lit_level;
  v.trail = trail_size (lit_level);
  v.reason = reason;
  assert ((int) num_assigned < max_var);
  assert (opts.reimply || num_assigned == trail.size ());
  num_assigned++;
  stats.walk.warmupset++;
  if (!lit_level && !from_external)
    learn_unit_clause (lit); // increases 'stats.fixed'
  const signed char tmp = sign (lit);
  phases.target[idx] = tmp;
  vals[idx] = tmp;
  vals[-idx] = -tmp;
  assert (val (lit) > 0);
  assert (val (-lit) < 0);

  if (!opts.reimply || level == 0) {
    trail.push_back (lit);
    return;
  }
  assert (level > 0 && trails.size () >= (size_t) level);
  trails[level - 1].push_back (lit);
#ifdef LOGGING
  if (!lit_level)
    LOG ("root-level unit assign %d @ 0", lit);
  else
    LOG (reason, "search assign %d @ %d", lit, lit_level);
#endif

  if (watching ()) {
    const Watches &ws = watches (-lit);
    if (!ws.empty ()) {
      const Watch &w = ws[0];
      __builtin_prefetch (&w, 0, 1);
    }
  }
  lrat_chain.clear ();
}


void Internal::warmup_propagate () {

  assert (!unsat);

  START (propagate);

  int64_t before = propagated;

  while (propagated != trail.size ()) {

    const int lit = -trail[propagated++];
    LOG ("propagating %d", -lit);
    Watches &ws = watches (lit);

    const const_watch_iterator eow = ws.end ();
    watch_iterator j = ws.begin ();
    const_watch_iterator i = j;

    while (i != eow) {

      const Watch w = *j++ = *i++;
      const signed char b = val (w.blit);

      if (b > 0)
        continue; // blocking literal satisfied

      if (w.binary ()) {

        // In principle we can ignore garbage binary clauses too, but that
        // would require to dereference the clause pointer all the time with
        //
        // if (w.clause->garbage) { j--; continue; } // (*)
        //
        // This is too costly.  It is however necessary to produce correct
        // proof traces if binary clauses are traced to be deleted ('d ...'
        // line) immediately as soon they are marked as garbage.  Actually
        // finding instances where this happens is pretty difficult (six
        // parallel fuzzing jobs in parallel took an hour), but it does
        // occur.  Our strategy to avoid generating incorrect proofs now is
        // to delay tracing the deletion of binary clauses marked as garbage
        // until they are really deleted from memory.  For large clauses
        // this is not necessary since we have to access the clause anyhow.
        //
        // Thanks go to Mathias Fleury, who wanted me to explain why the
        // line '(*)' above was in the code. Removing it actually really
        // improved running times and thus I tried to find concrete
        // instances where this happens (which I found), and then
        // implemented the described fix.

        // Binary clauses are treated separately since they do not require
        // to access the clause at all (only during conflict analysis, and
        // there also only to simplify the code).

        if (b < 0)
          ;// conflict = w.clause; // ignoring conflict
        else {
          build_chain_for_units (w.blit, w.clause, 0);
          warmup_assign (w.blit, w.clause);
          // lrat_chain.clear (); done in search_assign
        }

      } else {
        assert (w.clause->size > 2);

        // The cache line with the clause data is forced to be loaded here
        // and thus this first memory access below is the real hot-spot of
        // the solver.  Note, that this check is positive very rarely and
        // thus branch prediction should be almost perfect here.

        if (w.clause->garbage) {
          j--;
          continue;
        }
        if (w.clause == ignore)
          continue;
        literal_iterator lits = w.clause->begin ();
        const int other = lits[0] ^ lits[1] ^ lit;
        const signed char u = val (other);
        if (u > 0)
          j[-1].blit = other;
        else {
          const int size = w.clause->size;
          const const_literal_iterator end = lits + size;
          const literal_iterator middle = lits + w.clause->pos;
          literal_iterator k = middle;
          signed char v = -1;
          int r = 0;
          while (k != end && (v = val (r = *k)) < 0)
            k++;
          if (v < 0) {
            k = lits + 2;
            assert (w.clause->pos <= size);
            while (k != middle && (v = val (r = *k)) < 0)
              k++;
          }

          w.clause->pos = k - lits; // always save position

          assert (lits + 2 <= k), assert (k <= w.clause->end ());

          if (v > 0) {

            // Replacement satisfied, so just replace 'blit'.

            j[-1].blit = r;

          } else if (!v) {

            // Found new unassigned replacement literal to be watched.

            LOG (w.clause, "unwatch %d in", lit);

            lits[0] = other;
            lits[1] = r;
            *k = lit;

            watch_literal (r, lit, w.clause);

            j--; // Drop this watch from the watch list of 'lit'.

          } else if (!u) {

            assert (v < 0);

            // The other watch is unassigned ('!u') and all other literals
            // assigned to false (still 'v < 0'), thus we found a unit.
            //
            build_chain_for_units (other, w.clause, 0);
            warmup_assign (other, w.clause);
            // lrat_chain.clear (); done in search_assign

            // Similar code is in the implementation of the SAT'18 paper on
            // chronological backtracking but in our experience, this code
            // first does not really seem to be necessary for correctness,
            // and further does not improve running time either.
            //
            if (opts.chrono > 1) {

              const int other_level = var (other).level;

              if (other_level > var (lit).level) {

                // The assignment level of the new unit 'other' is larger
                // than the assignment level of 'lit'.  Thus we should find
                // another literal in the clause at that higher assignment
                // level and watch that instead of 'lit'.

                assert (size > 2);

                int pos, s = 0;

                for (pos = 2; pos < size; pos++)
                  if (var (s = lits[pos]).level == other_level)
                    break;

                assert (s);
                assert (pos < size);

                LOG (w.clause, "unwatch %d in", lit);
                lits[pos] = lit;
                lits[0] = other;
                lits[1] = s;
                watch_literal (s, other, w.clause);

                j--; // Drop this watch from the watch list of 'lit'.
              }
            }
          } else {

            assert (u < 0);
            assert (v < 0);

	    // ignoring conflict
          }
        }
      }
    }
  }

  stats.walk.warmupset += (trail.size() - before);
  STOP (propagate);
}

void Internal::warmup () {
  assert (!unsat);
  assert (!level);
  if (!opts.warmup)
    return;
  ++stats.walk.warmup;
  int res = 0;

  LOG ("propagating beyond conflicts to warm-up walk");
  while (!res && num_assigned < (size_t) max_var) {
    res = decide ();
    ++stats.walk.warmupset;
    warmup_propagate();
  }
  backtrack ();
}

}