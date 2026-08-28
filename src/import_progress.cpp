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
 * import_progress.cpp - the live display (see import_progress.hpp)
 */

#include "import_progress.hpp"

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

namespace
{
  /* ------------------------------------------------------------------ state */

  struct load_file
  {
    std::string label;		/* what the operator sees (the file's basename) */
    std::string path;		/* absolute, as handed to the child; matched against /proc/<pid>/fd */
    off_t size = 0;
    pid_t pid = 0;		/* 0 = not started */
    int fd = -1;		/* the child's fd on `path`, once found */
    off_t pos = 0;		/* highest offset seen (see read_child_offset) */
    bool done = false;
  };

  struct state
  {
    bool live = false;
    bool color = true;
    bool unicode = true;

    std::string database;
    std::string dump_dir;

    struct timespec t_start = { 0, 0 };
    struct timespec t_last_draw = { 0, 0 };

    bool in_phase = false;
    cubimport::progress::phase cur = cubimport::progress::phase::DISCOVER;
    std::string detail;

    bool has_counter = false;
    int done = 0;
    int total = 0;
    std::string item;

    bool loading = false;
    std::vector<load_file> files;
    int degree = 1;
    off_t total_bytes = 0;
    struct timespec t_load_start = { 0, 0 };

    int spin = 0;
    int drawn = 0;		/* lines the block currently occupies on screen */
  };

  state g;

  const char *PHASE_LABEL[] = {
    "discover", "define", "graph", "plan", "strip",
    "load", "rebuild", "fk define", "stats", "triggers"
  };
  static_assert (sizeof (PHASE_LABEL) / sizeof (PHASE_LABEL[0])
		 == (size_t) cubimport::progress::phase::COUNT, "PHASE_LABEL must match progress::phase");

  /* ------------------------------------------------------------- primitives */

  double
  since (const struct timespec &t)
  {
    struct timespec ts;
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return (double) (ts.tv_sec - t.tv_sec) + (double) (ts.tv_nsec - t.tv_nsec) / 1e9;
  }

  void
  stamp (struct timespec &t)
  {
    clock_gettime (CLOCK_MONOTONIC, &t);
  }

  int
  term_width ()
  {
    struct winsize ws;
    if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
      {
	return std::min (200, std::max (40, (int) ws.ws_col));
      }
    const char *cols = getenv ("COLUMNS");
    if (cols != NULL)
      {
	int c = atoi (cols);
	if (c > 0)
	  {
	    return std::min (200, std::max (40, c));
	  }
      }
    return 80;
  }

  /*
   * A display line under construction. ANSI escapes and multi-byte glyphs make
   * the byte length useless for layout, so columns are counted as they are
   * appended rather than measured afterwards.
   */
  struct linebuf
  {
    std::string s;
    int cols = 0;
    /* Hard column budget. Every redraw moves the cursor up by the number of
     * lines it last wrote, so a line that exceeds the terminal width and WRAPS
     * costs one physical row the arithmetic does not know about - and the erase
     * then leaves a stripe of stale block on screen. The layout below already
     * fits, but "already fits" is a property of six separate width calculations;
     * a cap makes over-running structurally impossible instead. */
    int cap = 1000;

    void put (const char *txt, int c)
    {
      if (cols + c > cap)
	{
	  return;		/* a glyph is atomic: drop it rather than split it */
	}
      s += txt;
      cols += c;
    }
    void put (const std::string &txt)
    {
      const int room = cap - cols;	/* ASCII only - every caller passes ASCII */
      if (room <= 0)
	{
	  return;
	}
      if ((int) txt.size () <= room)
	{
	  s += txt;
	  cols += (int) txt.size ();
	}
      else
	{
	  s.append (txt, 0, (size_t) room);
	  cols = cap;
	}
    }
    void put (const std::string &txt, int c)
    {
      if (cols + c > cap)
	{
	  return;
	}
      s += txt;
      cols += c;
    }
    void esc (const char *seq)
    {
      if (g.color)
	{
	  s += seq;
	}
    }
    void pad_to (int c)
    {
      while (cols < c && cols < cap)
	{
	  s += ' ';
	  cols++;
	}
    }
  };

  const char *DIM = "\033[2m";
  const char *BOLD = "\033[1m";
  const char *BLUE = "\033[34m";
  const char *GREEN = "\033[32m";
  const char *RESET = "\033[0m";

  std::string
  fit (const std::string &s, size_t width)
  {
    if (s.size () <= width)
      {
	return s;
      }
    if (width <= 1)
      {
	return std::string (width, '.');
      }
    return s.substr (0, width - 1) + "~";
  }

  /* Object files share a long prefix and carry the class name at the END
   * ("<prefix>_<owner>.<class>_objects"), so truncating on the right hides the
   * one part that identifies the file. Elide on the left instead. */
  std::string
  fit_tail (const std::string &s, size_t width)
  {
    if (s.size () <= width)
      {
	return s;
      }
    if (width <= 1)
      {
	return std::string (width, '.');
      }
    return "~" + s.substr (s.size () - (width - 1));
  }

  std::string
  elapsed_str (double sec)
  {
    if (sec < 0)
      {
	sec = 0;
      }
    int t = (int) sec;
    char buf[32];
    if (t >= 3600)
      {
	snprintf (buf, sizeof (buf), "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
      }
    else
      {
	snprintf (buf, sizeof (buf), "%02d:%02d", t / 60, t % 60);
      }
    return buf;
  }

  std::string
  bytes_str (off_t n)
  {
    char buf[32];
    double d = (double) n;
    if (d >= 1024.0 * 1024.0 * 1024.0)
      {
	snprintf (buf, sizeof (buf), "%.1f GB", d / (1024.0 * 1024.0 * 1024.0));
      }
    else if (d >= 1024.0 * 1024.0)
      {
	snprintf (buf, sizeof (buf), "%.0f MB", d / (1024.0 * 1024.0));
      }
    else
      {
	snprintf (buf, sizeof (buf), "%.0f KB", d / 1024.0);
      }
    return buf;
  }

  /* A bar `width` columns wide, `frac` of it filled. */
  void
  put_bar (linebuf &lb, int width, double frac, const char *color)
  {
    if (width < 4)
      {
	width = 4;
      }
    frac = std::min (1.0, std::max (0.0, frac));
    int on = (int) (frac * width + 0.5);
    lb.esc (color);
    for (int i = 0; i < on; ++i)
      {
	lb.put (g.unicode ? "\xe2\x96\x88" : "#", 1);	/* U+2588 FULL BLOCK */
      }
    lb.esc (DIM);
    for (int i = on; i < width; ++i)
      {
	lb.put (g.unicode ? "\xe2\x96\x91" : "-", 1);	/* U+2591 LIGHT SHADE */
      }
    lb.esc (RESET);
  }

  const char *
  spinner ()
  {
    static const char *braille[] = {
      "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc",
      "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f"
    };
    static const char *ascii[] = { "|", "/", "-", "\\" };
    if (g.unicode)
      {
	return braille[g.spin % 10];
      }
    return ascii[g.spin % 4];
  }

  /* -------------------------------------------------- the child's file offset
   *
   * The loaders are separate `cub_admin loaddb -C` processes: they write to a log
   * file, not to a pipe we own, and they report nothing until they exit. Their
   * own read position on the object file is the only progress signal that exists,
   * and the kernel publishes it in /proc/<pid>/fdinfo/<fd>.
   *
   * The offset is taken as monotonic - the highest value seen for that file is
   * kept - so a loader that seeks back (a re-read of any kind) shows a stalled
   * bar rather than one that runs backwards. It also means the bar tracks the
   * READ of the object file, which leads the commit of its rows; at the very end
   * a file sits at 100% for as long as its last transaction takes.
   */
  off_t
  read_fdinfo_pos (pid_t pid, int fd)
  {
    char p[64];
    snprintf (p, sizeof (p), "/proc/%d/fdinfo/%d", (int) pid, fd);
    FILE *f = fopen (p, "r");
    if (f == NULL)
      {
	return -1;
      }
    char line[128];
    off_t pos = -1;
    while (fgets (line, sizeof (line), f) != NULL)
      {
	if (strncmp (line, "pos:", 4) == 0)
	  {
	    pos = (off_t) strtoll (line + 4, NULL, 10);
	    break;
	  }
      }
    fclose (f);
    return pos;
  }

  int
  find_child_fd (pid_t pid, const std::string &path)
  {
    char dirp[64];
    snprintf (dirp, sizeof (dirp), "/proc/%d/fd", (int) pid);
    DIR *d = opendir (dirp);
    if (d == NULL)
      {
	return -1;
      }
    int found = -1;
    struct dirent *e;
    while ((e = readdir (d)) != NULL)
      {
	if (e->d_name[0] < '0' || e->d_name[0] > '9')
	  {
	    continue;
	  }
	char link[PATH_MAX];
	char target[PATH_MAX];
	snprintf (link, sizeof (link), "%s/%s", dirp, e->d_name);
	ssize_t n = readlink (link, target, sizeof (target) - 1);
	if (n <= 0)
	  {
	    continue;
	  }
	target[n] = '\0';
	if (path == target)
	  {
	    found = atoi (e->d_name);
	    break;
	  }
      }
    closedir (d);
    return found;
  }

  void
  refresh_offsets ()
  {
    for (load_file &f : g.files)
      {
	if (f.done || f.pid == 0)
	  {
	    continue;
	  }
	if (f.fd < 0)
	  {
	    f.fd = find_child_fd (f.pid, f.path);
	    if (f.fd < 0)
	      {
		continue;	/* not open yet, or no /proc: no measurable extent */
	      }
	  }
	off_t pos = read_fdinfo_pos (f.pid, f.fd);
	if (pos < 0)
	  {
	    f.fd = -1;		/* closed or re-opened; look again next tick */
	    continue;
	  }
	if (pos > f.pos)
	  {
	    f.pos = pos;
	  }
      }
  }

  /* ------------------------------------------------------------------ draw */

  void
  erase ()
  {
    if (g.drawn <= 0)
      {
	return;
      }
    /* The cursor is parked at the end of the block's last line (nothing emits a
     * trailing newline), so: column 0, up to the first line, clear downwards. */
    fputs ("\r", stdout);
    if (g.drawn > 1)
      {
	printf ("\033[%dA", g.drawn - 1);
      }
    fputs ("\033[J", stdout);
    g.drawn = 0;
    fflush (stdout);
  }

  void
  draw ()
  {
    erase ();

    const int W = term_width ();
    std::vector<std::string> lines;

    /* ---- header: who, which phase, how far through, how long ---- */
    {
      linebuf lb;
      lb.cap = W - 1;
      lb.put (" ", 1);
      lb.esc (BOLD);
      lb.put ("importdb", 8);
      lb.esc (RESET);
      lb.put ("  ", 2);
      lb.put (fit (g.database, 24));

      const std::string right =
	std::string (PHASE_LABEL[(int) g.cur]) + "  ["
	+ std::to_string ((int) g.cur + 1) + "/" + std::to_string ((int) cubimport::progress::phase::COUNT)
	+ "]  " + elapsed_str (since (g.t_start));
      const int right_at = W - 1 - (int) right.size ();
      if (right_at > lb.cols + 2)
	{
	  lb.pad_to (right_at);
	  lb.esc (DIM);
	  lb.put (right);
	  lb.esc (RESET);
	}
      lines.push_back (lb.s);
    }

    /* ---- body ---- */
    if (g.loading)
      {
	refresh_offsets ();

	off_t done_bytes = 0;
	int files_done = 0, files_running = 0;
	for (const load_file &f : g.files)
	  {
	    if (f.done)
	      {
		done_bytes += f.size;
		files_done++;
	      }
	    else
	      {
		done_bytes += f.pos;
		if (f.pid != 0)
		  {
		    files_running++;
		  }
	      }
	  }
	const double frac = (g.total_bytes > 0) ? (double) done_bytes / (double) g.total_bytes : 0.0;
	const double secs = since (g.t_load_start);
	const double rate = (secs > 0.5) ? (double) done_bytes / secs : 0.0;

	linebuf lb;
	lb.cap = W - 1;
	lb.put ("  ", 2);
	/* right-hand text first, so the bar takes exactly what is left */
	char pct[16];
	snprintf (pct, sizeof (pct), " %3d%%  ", (int) (frac * 100.0 + 0.5));
	std::string tail = std::to_string (files_done) + "/" + std::to_string (g.files.size ()) + " done";
	if (files_running > 0)
	  {
	    tail += " \xc2\xb7 " + std::to_string (files_running) + " loading";
	  }
	if (rate > 0.0)
	  {
	    tail += " \xc2\xb7 " + bytes_str ((off_t) rate) + "/s";
	    if (frac > 0.02 && frac < 0.995)
	      {
		tail += " \xc2\xb7 ~" + elapsed_str (((double) g.total_bytes - (double) done_bytes) / rate) + " left";
	      }
	  }
	/* each "\xc2\xb7" is one column but two bytes; count them back out */
	const int tail_cols = (int) tail.size () - 1 * (int) std::count (tail.begin (), tail.end (), '\xb7');
	const int bar_w = W - 2 - (int) strlen (pct) - tail_cols - 2;
	put_bar (lb, bar_w, frac, GREEN);
	lb.put (pct);
	lb.esc (DIM);
	lb.put (tail, tail_cols);
	lb.esc (RESET);
	lines.push_back (lb.s);

	/* one line per loader in flight, up to a screenful's worth */
	int shown = 0;
	for (const load_file &f : g.files)
	  {
	    if (f.done || f.pid == 0 || shown >= 8)
	      {
		continue;
	      }
	    linebuf fb;
	    fb.cap = W - 1;
	    const int name_w = std::min (34, std::max (12, W / 3));
	    fb.put ("    ", 4);
	    fb.esc (DIM);
	    std::string nm = fit_tail (f.label, name_w);
	    fb.put (nm);
	    fb.pad_to (4 + name_w + 2);
	    fb.esc (RESET);
	    if (f.size > 0 && f.fd >= 0)
	      {
		const double ff = (double) f.pos / (double) f.size;
		char fp[48];
		snprintf (fp, sizeof (fp), " %3d%%  %s", (int) (ff * 100.0 + 0.5), bytes_str (f.size).c_str ());
		put_bar (fb, std::max (8, W - fb.cols - (int) strlen (fp) - 2), ff, BLUE);
		fb.esc (DIM);
		fb.put (fp);
		fb.esc (RESET);
	      }
	    else
	      {
		fb.esc (DIM);
		fb.put (spinner (), 1);
		fb.put (" reading", 8);
		fb.esc (RESET);
	      }
	    lines.push_back (fb.s);
	    shown++;
	  }
      }
    else if (g.has_counter && g.total > 0)
      {
	linebuf lb;
	lb.cap = W - 1;
	lb.put ("  ", 2);
	const double frac = (double) g.done / (double) g.total;
	char mid[64];
	snprintf (mid, sizeof (mid), "  %d/%d  ", g.done, g.total);
	const int item_w = std::max (0, W - 2 - 24 - (int) strlen (mid) - 2);
	put_bar (lb, 24, frac, GREEN);
	lb.put (mid);
	lb.esc (DIM);
	lb.put (fit (g.item, item_w));
	lb.esc (RESET);
	lines.push_back (lb.s);
      }
    else
      {
	linebuf lb;
	lb.cap = W - 1;
	lb.put ("  ", 2);
	lb.esc (BLUE);
	lb.put (spinner (), 1);
	lb.esc (RESET);
	lb.put (" ", 1);
	lb.esc (DIM);
	lb.put (fit (g.detail.empty () ? "working" : g.detail, (size_t) std::max (0, W - 6)));
	lb.esc (RESET);
	lines.push_back (lb.s);
      }

    for (size_t i = 0; i < lines.size (); ++i)
      {
	if (i > 0)
	  {
	    fputc ('\n', stdout);
	  }
	fputs (lines[i].c_str (), stdout);
      }
    /* no trailing newline: the cursor parks at the end of the last line, which
     * is where erase () expects to find it. */
    fflush (stdout);
    g.drawn = (int) lines.size ();
    stamp (g.t_last_draw);
  }
} // namespace

namespace cubimport
{
  namespace progress
  {

    bool
    parse_mode (const char *text, mode &out)
    {
      if (text == NULL)
	{
	  return false;
	}
      if (strcmp (text, "auto") == 0)
	{
	  out = mode::AUTO;
	  return true;
	}
      if (strcmp (text, "always") == 0 || strcmp (text, "yes") == 0 || strcmp (text, "on") == 0)
	{
	  out = mode::ALWAYS;
	  return true;
	}
      if (strcmp (text, "never") == 0 || strcmp (text, "no") == 0 || strcmp (text, "off") == 0)
	{
	  out = mode::NEVER;
	  return true;
	}
      return false;
    }

    void
    configure (mode m)
    {
      if (m == mode::NEVER)
	{
	  g.live = false;
	  return;
	}
      if (m == mode::AUTO)
	{
	  /* A pipe, a file, a CI log or a `TERM=dumb` shell gets the plain output
	   * it got before this file existed - which is also what keeps the
	   * functional suite's stdout assertions meaningful. */
	  const char *term = getenv ("TERM");
	  if (!isatty (STDOUT_FILENO) || term == NULL || strcmp (term, "dumb") == 0)
	    {
	      g.live = false;
	      return;
	    }
	}
      g.live = true;

      g.color = (getenv ("NO_COLOR") == NULL);
      const char *lc = getenv ("LC_ALL");
      if (lc == NULL || lc[0] == '\0')
	{
	  lc = getenv ("LC_CTYPE");
	}
      if (lc == NULL || lc[0] == '\0')
	{
	  lc = getenv ("LANG");
	}
      g.unicode = (lc != NULL && (strstr (lc, "UTF-8") != NULL || strstr (lc, "utf8") != NULL
				  || strstr (lc, "UTF8") != NULL || strstr (lc, "utf-8") != NULL));
    }

    bool
    live ()
    {
      return g.live;
    }

    void
    begin_run (const std::string &database, const std::string &dump_dir)
    {
      g.database = database;
      g.dump_dir = dump_dir;
      stamp (g.t_start);
    }

    void
    begin_phase (phase p)
    {
      if (!g.live)
	{
	  return;
	}
      g.cur = p;
      g.in_phase = true;
      g.has_counter = false;
      g.loading = false;
      g.detail.clear ();
      g.item.clear ();
      g.done = g.total = 0;
      draw ();
    }

    void
    set_detail (const std::string &text)
    {
      if (!g.live || !g.in_phase)
	{
	  return;
	}
      g.detail = text;
      tick ();
    }

    void
    set_counter (int done, int total, const std::string &item)
    {
      if (!g.live || !g.in_phase)
	{
	  return;
	}
      g.has_counter = true;
      g.done = done;
      g.total = total;
      g.item = item;
      tick ();
    }

    void
    load_begin (const std::vector<std::string> &files, const std::vector<off_t> &sizes, int degree)
    {
      if (!g.live)
	{
	  return;
	}
      g.loading = true;
      g.degree = degree;
      g.files.clear ();
      g.total_bytes = 0;
      for (size_t i = 0; i < files.size (); ++i)
	{
	  load_file f;
	  f.path = files[i];
	  size_t slash = f.path.find_last_of ('/');
	  f.label = (slash == std::string::npos) ? f.path : f.path.substr (slash + 1);
	  const std::string suffix = "_objects";
	  if (f.label.size () > suffix.size ()
	      && f.label.compare (f.label.size () - suffix.size (), suffix.size (), suffix) == 0)
	    {
	      f.label.resize (f.label.size () - suffix.size ());
	    }
	  f.size = (i < sizes.size ()) ? sizes[i] : 0;
	  g.total_bytes += f.size;
	  g.files.push_back (f);
	}
      stamp (g.t_load_start);
      draw ();
    }

    void
    load_child (size_t idx, pid_t pid)
    {
      if (!g.live || idx >= g.files.size ())
	{
	  return;
	}
      g.files[idx].pid = pid;
      g.files[idx].fd = -1;
      draw ();
    }

    void
    load_child_done (size_t idx)
    {
      if (!g.live || idx >= g.files.size ())
	{
	  return;
	}
      g.files[idx].done = true;
      draw ();
    }

    void
    load_end ()
    {
      if (!g.live)
	{
	  return;
	}
      g.loading = false;
      g.has_counter = false;
      g.detail.clear ();
      draw ();
    }

    void
    tick ()
    {
      if (!g.live || !g.in_phase)
	{
	  return;
	}
      if (since (g.t_last_draw) < 0.12)
	{
	  return;
	}
      g.spin++;
      draw ();
    }

    void
    end_phase ()
    {
      if (!g.live)
	{
	  return;
	}
      erase ();
      g.in_phase = false;
      g.loading = false;
      g.has_counter = false;
    }

    void
    hide ()
    {
      if (!g.live)
	{
	  return;
	}
      erase ();
    }

    void
    finish ()
    {
      if (!g.live)
	{
	  return;
	}
      erase ();
      g.in_phase = false;
      g.live = false;
    }

  }				// namespace progress
} // namespace cubimport
