/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * import_progress.hpp - the live display
 *
 * A full-database reload is minutes of silence under the old output: each phase
 * prints one line when it is already over, and the Load phase - the one that
 * actually takes the time - prints nothing at all until every child has exited.
 * An operator watching it cannot tell a slow import from a stuck one.
 *
 * This draws a small block at the bottom of the terminal, redrawn in place a few
 * times a second, that answers the three questions the printed lines cannot:
 * which phase is running (and how many are left), how far into it we are, and
 * how long it has taken. During the data phase it goes further and reports each
 * loader's position in its object file, read from /proc/<pid>/fdinfo - the
 * children are separate `cub_admin loaddb -C` processes with no channel back, so
 * their own file offset is the only honest progress signal available.
 *
 * Three properties are deliberate:
 *
 *   - It is OFF unless stdout is a terminal. The functional suite, the demo and
 *     CI all parse this tool's stdout; with the display off, every byte written
 *     is what it was before this file existed. `--progress` overrides in both
 *     directions.
 *   - It owns no thread. The client is a forking process (the load pool) and a
 *     background redraw thread would be holding the malloc lock across fork ()
 *     exactly when it must not. Redraws happen on the main thread from tick (),
 *     which the phases call as they work.
 *   - Anything else that writes must call hide () first, or the next redraw
 *     scrolls over it. The IMPORT_PRINT / IMPORT_ERR macros below are the two
 *     output idioms in this codebase with that call folded in, so a phase cannot
 *     forget; the phase modules use them instead of fprintf / PRINT_AND_LOG_ERR_MSG.
 */

#ifndef _IMPORT_PROGRESS_HPP_
#define _IMPORT_PROGRESS_HPP_

#include <sys/types.h>

#include <string>
#include <vector>

namespace cubimport
{
  namespace progress
  {

    /* --progress=auto|always|never; AUTO means "on when stdout is a terminal". */
    enum class mode
    {
      AUTO,
      ALWAYS,
      NEVER
    };

    /* The pipeline's phases, in order, as the [k/n] counter presents them. */
    enum class phase
    {
      DISCOVER = 0,
      DEFINE,
      GRAPH,
      PLAN,
      STRIP,
      LOAD,
      REBUILD,
      FKDEFINE,
      STATS,
      TRIGGERS,
      COUNT
    };

    /* Parse a --progress value; returns false (and leaves out untouched) for an
     * unrecognized one so the caller can reject it by name. */
    bool parse_mode (const char *text, mode &out);

    /* Decide once whether a live display runs at all: NEVER never, ALWAYS
     * always, AUTO when stdout is a terminal that can carry it. Everything below
     * is a no-op when the answer is no. */
    void configure (mode m);
    bool live ();

    /* Name the run (drawn in the header) and start the elapsed clock. */
    void begin_run (const std::string &database, const std::string &dump_dir);

    /* Enter a phase. Redraws immediately, so the block appears as work starts
     * rather than after the first tick (). */
    void begin_phase (phase p);

    /* One line of phase-specific state under the header, for phases with no
     * measurable extent (define, graph, plan, strip). */
    void set_detail (const std::string &text);

    /* k-of-n progress for the phases that walk a known list (rebuild, fk define,
     * stats, triggers): `done` of `total`, currently working on `item`. */
    void set_counter (int done, int total, const std::string &item);

    /* ---- the data phase ---- */

    /* The object files this run will load, and the pool's degree. Sizes are the
     * files' byte sizes; a size of 0 (unstattable) just means that file
     * contributes no measurable extent. */
    void load_begin (const std::vector<std::string> &files, const std::vector<off_t> &sizes, int degree);
    void load_child (size_t idx, pid_t pid);	/* a loader started on files[idx] */
    void load_child_done (size_t idx);	/* ... and exited */

    /* The pool is drained. Leaves the per-file view without leaving the phase,
     * so the catalog row counts that follow still have somewhere to report. */
    void load_end ();

    /* Redraw, rate-limited internally (~8/s). Cheap enough to call in a wait
     * loop; a no-op when the display is off. */
    void tick ();

    /* Leave the phase and erase the block, so the phase's own summary line
     * prints on a clean terminal. */
    void end_phase ();

    /* Erase the block if it is up. Idempotent, and a no-op when the display is
     * off; the next tick () or begin_phase () draws it again. */
    void hide ();

    /* End of run: erase for good and stop drawing. */
    void finish ();

  }				// namespace progress
} // namespace cubimport

/*
 * The two output idioms, with the erase folded in. Phase modules must use these
 * rather than fprintf (stdout, ...) / PRINT_AND_LOG_ERR_MSG (...) directly: a
 * line written while the block is up is scrolled over by the next redraw, which
 * is how an error message disappears from an operator's screen. Both expand to
 * exactly the original call when the display is off.
 */
#define IMPORT_PRINT(...) \
  do { cubimport::progress::hide (); fprintf (stdout, __VA_ARGS__); } while (0)

#define IMPORT_ERR(...) \
  do { cubimport::progress::hide (); PRINT_AND_LOG_ERR_MSG (__VA_ARGS__); } while (0)

#define IMPORT_WARN(...) \
  do { cubimport::progress::hide (); fprintf (stderr, __VA_ARGS__); } while (0)

#endif /* _IMPORT_PROGRESS_HPP_ */
