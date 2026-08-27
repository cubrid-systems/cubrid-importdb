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
 * import_db.cpp - Main for the importdb utility
 *
 * importdb is an admin orchestrator that reuses the loaddb machinery to import
 * a database dump into a running server. WU-10 landed the CLI surface; WU-11
 * added Stage 1 (Discovery, filesystem-only) + the manifest (WU-12); WU-20/21
 * add the server-side stages that run on ONE shared CS-mode session opened here
 * (import_session): Stage 2 Definition (define the empty target) then the Graph
 * builder's catalog snapshot (build the DependencyGraph, Q11-gated). WU-22 adds
 * Stage 3 (Planner): a PURE-COMPUTATION pass over the in-memory graph that builds
 * the Schedule (data-phase level sets + terminal task list), machine-checks its
 * ordering, and records it in the manifest. WU-23 adds --dry-run: run the whole
 * plan-building pipeline (define -> graph -> schedule -> print) but ABORT the
 * transaction at the close instead of committing (CUBRID DDL is transactional, so
 * the defined schema is rolled back and the target is left untouched) and skip
 * every manifest write, so a dry run leaves no persistent artifact while printing
 * exactly the plan a real run would execute. WU-30 adds the Strip phase, the
 * first EXECUTION step of the §6 constraint lifecycle, on the normal (non-dry)
 * path only: after the schedule print it DROPs the graph's PK/UK/FK on the empty
 * tables (FKs, then uniques, then PKs) so the data phase loads into bare heaps,
 * records the stripped set in the manifest (STRIPPED phase - the Gap-R resume
 * basis). WU-31 then loads each table's object data into the bare heaps (LOADED
 * phase), serially in-process by default or - with --degree > 1 (WU-40) - through
 * a bounded pool of independent `loaddb -C` subprocesses that load independent
 * object files concurrently (the strip is committed first so those separate
 * connections see the bare heaps). WU-32 rebuilds the terminal constraints: re-add the stripped PK/UNIQUE
 * and build the deferred plain indexes on the now-populated tables (REBUILT
 * phase), enforcing the §6 rebuild-failure contract - a PK/UNIQUE that fails on a
 * duplicate key is left un-rebuilt, recorded in the manifest pending-rebuild set,
 * its parent-side FK edges recorded as withheld (WU-34 honors them), and the run
 * continues then exits non-zero while still committing the partial result. WU-33
 * then re-validates every FK edge by a set-based anti-join BEFORE the FK is
 * defined (CUBRID's ADD FOREIGN KEY does not check existing rows, so the anti-join
 * IS the validation): default fail-fast stops at the first edge with an orphan,
 * --continue enumerates every offending row, and any orphan writes the exceptions
 * artifact + [validate] manifest section and makes the run exit non-zero while the
 * loaded data + rebuilt constraints still commit. The FK define phase then
 * defines the FK on every validated-clean edge by re-executing the dump's own
 * ADD ... FOREIGN KEY statements (bulk-building the FK b-tree on the populated
 * tables), the terminal step of the §6 constraint lifecycle: an edge WU-32
 * withheld or WU-33 found violated is left undefined and recorded with its re-add
 * DDL (manifest [fkdefine] + exceptions artifact) for an operator, while every
 * clean edge's FK is defined (FK_DEFINED phase). WU-35 then finishes the two
 * terminal tasks: it refreshes the per-class statistics on the loaded + rebuilt
 * tables (STATS_UPDATED phase, reusing loaddb's per-class sm_update_statistics
 * path) and, strictly last, defines the dump's deferred triggers so none fires on
 * the bulk-loaded rows (DONE phase) - the functionally-complete serial import. A
 * clean run ends with catalog == snapshot - every PK/UK/FK/index restored (the
 * full round-trip), statistics present per class, and triggers defined -
 * committing and exiting 0; a run with any withheld FK, failed statistics
 * update, or failed trigger define commits the partial result and exits non-zero.
 * WU-36 then prints the consolidated RunReport (10-design.md §7 / import_report):
 * verdict, planned order, per-class done/pending/skipped state, loaded rows, and
 * the pending-rebuild + withheld-FK repair records with their re-add DDL - the
 * operator's single-glance run outcome (G5), a pure pass over the run's summaries.
 * WU-50 then makes the run RESUMABLE (Gap-R): each phase commits before its
 * manifest marker advances, so the manifest is a record of durable work rather
 * than of intentions, and re-running the same command after a kill reads that
 * manifest back (graph, stripped set, per-phase records), skips every phase it
 * reports complete, and re-enters the one phase that could have been
 * interrupted under a guard that skips what is already done - a fully DONE
 * manifest is a no-op, and --restart ignores the manifest entirely.
 */

#include "config.h"

#include "porting.h"
#include "utility.h"
#include "message_catalog.h"
#include "util_func.h"
#include "import_discovery.hpp"
#include "import_manifest.hpp"
#include "import_resume.hpp"
#include "import_session.hpp"

#include "dbi.h"			/* db_get_ha_server_state - the WU-53 guard */
#include "boot.h"			/* HA_SERVER_STATE */
#include "system_parameter.h"	/* prm_get_integer_value - HA_DISABLED needs it */
#include "connection_defs.h"	/* HA_DISABLED - the same gate loaddb's fallback uses */
#include "import_define.hpp"
#include "import_graph.hpp"
#include "import_plan.hpp"
#include "import_strip.hpp"
#include "import_load.hpp"
#include "import_rebuild.hpp"
#include "import_validate.hpp"
#include "import_fkdefine.hpp"
#include "import_stats.hpp"
#include "import_triggers.hpp"
#include "import_report.hpp"

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

/*
 * importdb() - importdb main routine
 *   return: EXIT_SUCCESS/EXIT_FAILURE
 */
int
importdb (UTIL_FUNCTION_ARG *arg)
{
  UTIL_ARG_MAP *arg_map = arg->arg_map;
  /* Initialised, not merely declared: the error_exit path reads them to report
   * the resume point, and the argument-parsing failures jump there first. */
  const char *database_name = NULL;
  const char *dump_dir = NULL;
  const char *user_name;
  const char *password;
  const char *exceptions_table;
  int degree;
  bool continue_on_error;
  bool skip_object_classes;
  bool dry_run;
  bool restart;
  bool allow_ha;
  bool target_is_ha = false;
  /* EXIT_SUCCESS unless the Rebuild phase (WU-32) recorded a pending-rebuild:
   * a partial-but-committed run exits non-zero with the manifest as the record. */
  int exit_code = EXIT_SUCCESS;

  /* database-name and dump-dir are the two required positionals. */
  if (utility_get_option_string_table_size (arg_map) != 2)
    {
      goto print_import_usage;
    }

  database_name = utility_get_option_string_value (arg_map, OPTION_STRING_TABLE, 0);
  dump_dir = utility_get_option_string_value (arg_map, OPTION_STRING_TABLE, 1);
  if (database_name == NULL || dump_dir == NULL)
    {
      goto print_import_usage;
    }

  user_name = utility_get_option_string_value (arg_map, IMPORT_USER_S, 0);
  password = utility_get_option_string_value (arg_map, IMPORT_PASSWORD_S, 0);
  degree = utility_get_option_int_value (arg_map, IMPORT_DEGREE_S);
  continue_on_error = utility_get_option_bool_value (arg_map, IMPORT_CONTINUE_S);
  skip_object_classes = utility_get_option_bool_value (arg_map, IMPORT_SKIP_OBJECT_CLASSES_S);
  dry_run = utility_get_option_bool_value (arg_map, IMPORT_DRY_RUN_S);
  restart = utility_get_option_bool_value (arg_map, IMPORT_RESTART_S);
  allow_ha = utility_get_option_bool_value (arg_map, IMPORT_ALLOW_HA_S);
  exceptions_table = utility_get_option_string_value (arg_map, IMPORT_EXCEPTIONS_TABLE_S, 0);

  /* D4 forward-compat: --exceptions-table is reserved and always errors out. */
  if (exceptions_table != NULL)
    {
      PRINT_AND_LOG_ERR_MSG (msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
					     IMPORTDB_MSG_EXCEPTIONS_TABLE_RESERVED));
      goto error_exit;
    }

  /* WU-40: --degree drives the inter-table parallel data phase - degree > 1
   * loads independent object files concurrently through a bounded `loaddb -C`
   * subprocess pool (the data phase only; every other phase runs serially on the
   * one shared session). A degree below 1 is normalized to serial. */
  if (degree < 1)
    {
      degree = 1;
    }

  if (check_database_name (database_name))
    {
      goto error_exit;
    }

  /* WU-11 Stage 1: roster + validate the dump directory into an ImportSet. On
   * a validation failure discover() has already emitted the named diagnostic.
   * No server connection or data load happens here - those are later WUs. */
  {
    cubimport::import_set iset;
    if (cubimport::discover (database_name, dump_dir, iset) != cubimport::discover_status::OK)
      {
	goto error_exit;
      }
    cubimport::print_import_set_summary (iset);

    const std::string manifest_path = iset.dump_dir + "/importdb.manifest";

    /* WU-50 (Gap-R): a manifest left in the dump directory is the record of a
     * prior run of THIS dump into THIS database. Auto-detect what to do with it
     * - re-running the same command is how an operator retries, so nothing new
     * has to be typed - guarded by the [set] identity so an unrelated manifest
     * never drives the run. --restart is the escape hatch that ignores it.
     *
     * A DONE manifest means there is nothing to do. An INCOMPLETE one means a
     * killed run, and the phases it records complete are skipped: the marker is
     * advanced only AFTER the phase's work is committed (see the phase-boundary
     * commits below), so "reached X" means X is durable. The phase right after
     * `reached` is the only one that can be half-done, and it is re-entered
     * under the resume guard. */
    cubimport::manifest_state prior;
    bool resuming = false;
    /* A refusal leaves a manifest this run is about to overwrite. It is the only
     * record of an interrupted import's progress, so it is moved aside first. */
    bool preserve_prior = false;

    if (!dry_run)
      {
	cubimport::manifest_state found;
	std::string read_error;
	const cubimport::manifest_read mr = cubimport::read_manifest (iset.dump_dir, found, read_error);

	if (mr == cubimport::manifest_read::UNREADABLE)
	  {
	    /* A manifest that exists and cannot be read is NOT "no manifest".
	     * Importing from scratch would define over a target that may already
	     * be half imported, and the operator would never learn a resume was
	     * available. */
	    PRINT_AND_LOG_ERR_MSG (msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						   IMPORTDB_MSG_MANIFEST_UNREADABLE), manifest_path.c_str (),
				   read_error.c_str ());
	    goto error_exit;
	  }

	if (mr == cubimport::manifest_read::OK)
	  {
	    if (restart)
	      {
		fprintf (stdout, msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						 IMPORTDB_MSG_RESTART_IGNORING), manifest_path.c_str (),
			 iset.database_name.c_str ());
		preserve_prior = true;
	      }
	    else if (found.format_version > cubimport::manifest_format_version ())
	      {
		/* Before the corrupt check below: a future format that adds a
		 * phase writes a `current:` this build cannot name, which leaves
		 * has_phase false and would be misreported as truncation. The
		 * version comes from the preamble, so it is readable whatever
		 * [phases] holds. */
		fprintf (stdout, msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						 IMPORTDB_MSG_RESUME_NOT_POSSIBLE), manifest_path.c_str (),
			 ("it was written in format v" + std::to_string (found.format_version)
			  + " by a newer importdb, and this build reads up to v"
			  + std::to_string (cubimport::manifest_format_version ())).c_str (),
			 iset.database_name.c_str ());
		preserve_prior = true;
	      }
	    else if (!found.has_phase)
	      {
		/* The file is there but records no phase - truncated or corrupt.
		 * `reached` would default to DISCOVERED, which is a real phase,
		 * and the refusal for THAT case tells the operator to drop the
		 * partial schema. Saying that about a database that may be fully
		 * loaded is worse than saying nothing, so refuse without writing
		 * or touching anything. */
		PRINT_AND_LOG_ERR_MSG (msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						       IMPORTDB_MSG_MANIFEST_CORRUPT), manifest_path.c_str ());
		goto error_exit;
	      }
	    else if (found.reached == cubimport::import_phase::DONE)
	      {
		/* Re-running importdb on a dump this exact target already fully
		 * imported is a no-op rather than a failure on re-defining the
		 * existing schema. But "every phase ran" is not "the import
		 * succeeded": a run that left a PK un-rebuilt on duplicate keys,
		 * withheld an FK over orphan rows, or failed a trigger also
		 * reaches DONE, and it exited non-zero saying so. Re-derive that
		 * verdict from the records rather than reporting success a second
		 * time - otherwise re-running the command silently launders a
		 * failed import into exit 0, and any tooling that retries then
		 * believes it. */
		const int unresolved_keys = (int) (found.rebuild.pending.size () + found.rebuild.failed_indexes.size ());
		const int unresolved_fks = (int) found.fkdefine.withheld.size ();
		const int unresolved_stats = (int) found.stats.failed.size ();
		const int unresolved_triggers = found.triggers.failed;
		/* Parity with the live exit-code expression, which also fails on a
		 * violated edge. A violated edge is normally withheld too, so
		 * unresolved_fks covers it - but only if the FK's statement was
		 * matched in the schema files, so check the tally directly rather
		 * than rest on that. */
		const int unresolved_orphans = found.validate.violated_edges;

		if (unresolved_keys == 0 && unresolved_fks == 0 && unresolved_stats == 0 && unresolved_triggers == 0
		    && unresolved_orphans == 0)
		  {
		    fprintf (stdout, msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						     IMPORTDB_MSG_ALREADY_DONE), iset.database_name.c_str (),
			     manifest_path.c_str ());
		    return EXIT_SUCCESS;
		  }

		PRINT_AND_LOG_ERR_MSG (msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						       IMPORTDB_MSG_ALREADY_DONE_PARTIAL), iset.database_name.c_str (),
				       unresolved_keys, unresolved_fks, unresolved_stats, unresolved_triggers,
				       manifest_path.c_str ());
		return EXIT_FAILURE;
	      }
	    else
	      {
		/* Everything a resume reconstructs has to actually be in the
		 * file. The DISCOVERED case is the one deliberate refusal: the
		 * killed run had not finished the definition phase, and the
		 * schema files commit as they run, so the target may hold a
		 * partial definition. Reconciling that would mean dropping user
		 * classes on a database whose contents importdb never recorded -
		 * including the case where define failed precisely BECAUSE the
		 * target was not empty. Defining is also the cheapest phase to
		 * redo. So it is reported, not repaired. */
		std::string why;
		if (found.database != iset.database_name || found.prefix != iset.prefix)
		  {
		    why = "it records a different database or dump prefix";
		  }
		else if (found.reached == cubimport::import_phase::DISCOVERED)
		  {
		    why = "the interrupted run had not finished defining the target, so no phase is complete to "
			  "resume from; drop the partial schema (or use an empty target) and re-run";
		  }
		else if (found.reached >= cubimport::import_phase::STRIPPED && !found.has_graph)
		  {
		    /* Past the Strip phase the catalog no longer holds the PK/UK/FK
		     * the graph is made of, so a manifest without the serialized
		     * graph (one written before WU-50) has nothing to rebuild it
		     * from. At DEFINED it is a different story - see below. */
		    why = "it predates the serialized dependency graph, which cannot be rebuilt from a stripped "
			  "catalog";
		  }
		else if (found.reached >= cubimport::import_phase::STRIPPED && !found.has_strip)
		  {
		    why = "its [strip] record - the rebuild input - is missing";
		  }
		else if (found.reached >= cubimport::import_phase::LOADED && !found.has_load)
		  {
		    why = "its [load] record is missing";
		  }
		else if (found.reached >= cubimport::import_phase::REBUILT && !found.has_rebuild)
		  {
		    why = "its [rebuild] record is missing";
		  }
		else if (found.reached >= cubimport::import_phase::FK_DEFINED && !found.has_validate)
		  {
		    why = "its [validate] record is missing";
		  }
		else if (found.reached >= cubimport::import_phase::FK_DEFINED && !found.has_fkdefine)
		  {
		    why = "its [fkdefine] record - which carries the withheld FKs' re-add DDL - is missing";
		  }
		else if (found.reached >= cubimport::import_phase::STATS_UPDATED && !found.has_stats)
		  {
		    why = "its [stats] record is missing";
		  }

		if (!why.empty ())
		  {
		    fprintf (stdout, msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						     IMPORTDB_MSG_RESUME_NOT_POSSIBLE), manifest_path.c_str (),
			     why.c_str (), iset.database_name.c_str ());
		    preserve_prior = true;
		  }
		else
		  {
		    prior = found;
		    resuming = true;
		    fprintf (stdout, msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						     IMPORTDB_MSG_RESUME_START), iset.database_name.c_str (),
			     manifest_path.c_str (), cubimport::phase_name (prior.reached));
		  }
	      }
	  }
      }

    /* A resumed run makes none of the manifest writes that precede the phase it
     * re-enters - rewriting the manifest would erase the record it is resuming
     * from. The one exception is a resume that starts at DEFINED: nothing later
     * has been recorded yet, so re-stamping DEFINED loses nothing and gains the
     * serialized graph for a second kill. */
    const bool rewrite_defined = !resuming || prior.reached == cubimport::import_phase::DEFINED;

    /* WU-12 Manifest v0: write the plan/progress manifest into the importdb
     * directory (crash-safe temp+rename). It records the rostered set, a
     * planned-order placeholder (real order is WU-22), and the phase markers
     * (only "discovered" reached). A write failure emits a named diagnostic
     * and fails the run. Still no server connection or data load here. A
     * --dry-run leaves no persistent artifact, so every manifest write below is
     * skipped when dry_run is set - and so is EVERY write a resumed run would
     * make before the phase it re-enters: rewriting the manifest here would
     * erase the record the resume is reading from, leaving a second kill with
     * nothing to resume. */
    if (!dry_run && !resuming)
      {
	/* This run is about to replace the manifest. When it got here by
	 * REFUSING to resume - a mistyped database name, --restart, an
	 * incomplete record - the file it replaces is the only trace of the
	 * interrupted import's progress, and losing it is the difference between
	 * re-running and reconstructing by hand. Move it aside first. */
	if (preserve_prior)
	  {
	    std::string saved;
	    if (cubimport::preserve_manifest (iset.dump_dir, saved))
	      {
		fprintf (stdout, msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						 IMPORTDB_MSG_MANIFEST_PRESERVED), saved.c_str (),
			 manifest_path.c_str ());
	      }
	    else
	      {
		/* Overwriting anyway would lose the record silently, which is the
		 * whole failure this preserve step exists to prevent. */
		PRINT_AND_LOG_ERR_MSG (msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						       IMPORTDB_MSG_MANIFEST_PRESERVE_FAILED), manifest_path.c_str (),
				       strerror (errno));
		goto error_exit;
	      }
	  }
	if (!cubimport::write_manifest (iset, cubimport::import_phase::DISCOVERED))
	  {
	    goto error_exit;
	  }
      }

    /* WU-20/21 run on ONE shared CS-mode session (10-design.md §4 lifecycle:
     * define -> snapshot on the same open connection). Open it once here,
     * DBA-group-gated; on failure session_open() has already emitted the named
     * diagnostic and shut the connection back down. */
    if (cubimport::session_open (arg->command_name, iset.database_name.c_str (), user_name, password)
	!= cubimport::session_status::OK)
      {
	goto error_exit;
      }

    /* WU-53 HA guard. The data phase loads into bare heaps -- strip removes
     * PK/UNIQUE/FK before it and rebuild puts them back after -- and the server
     * writes a row replication record only while walking a BTREE_PRIMARY_KEY index
     * (locator_sr.c:8038). With no PK in place during the load there is nothing to
     * replicate, so an HA standby receives the schema (define/rebuild/fkdefine are
     * statement-replicated) and none of the rows, and no error is raised anywhere.
     * A silently empty standby is worse than a refused import, so refuse by
     * default and make --allow-ha the explicit way to accept it. Asked of the
     * SERVER rather than read from this client's own cubrid.conf: what matters is
     * the state of the database being written to. */
    {
      /* HA_DISABLED() is HA_GET_MODE () == HA_MODE_OFF -- the same gate loaddb's
       * per-record fallback uses (load_server_loader.cpp:767). ha_mode carries
       * PRM_FORCE_SERVER, so a CS client sees the SERVER's value, which is the one
       * that decides whether replication log is produced at all. Do not use
       * db_get_ha_server_state (): a standalone non-HA server reports "active" too,
       * so it cannot tell HA from non-HA. */
      char ha_state_name[64] = "";
      if (!HA_DISABLED ())
	{
	  (void) db_get_ha_server_state (ha_state_name, sizeof (ha_state_name) - 1);
	  if (ha_state_name[0] == '\0')
	    {
	      strcpy (ha_state_name, "server");
	    }
	  target_is_ha = true;
	  if (!allow_ha)
	    {
	      PRINT_AND_LOG_ERR_MSG (msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						     IMPORTDB_MSG_HA_REFUSED), iset.database_name.c_str (),
				     ha_state_name);
	      cubimport::session_close (false);
	      goto error_exit;
	    }
	  PRINT_AND_LOG_ERR_MSG (msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						 IMPORTDB_MSG_HA_PROCEEDING), iset.database_name.c_str (),
				 ha_state_name);
	}
    }

    /* WU-20 Definition phase: execute the dump's definition DDL (classes,
     * columns, ADD SUPERCLASS, serials, partitions, PK/UK/FK) on the open
     * session - triggers and secondary indexes are DEFERRED to terminal WUs
     * and no data is loaded. The result is a fully-defined but empty target
     * DB. A normal run commits it durably; a --dry-run (commit=false) leaves
     * the DDL uncommitted so session_close(false) below rolls it back. On
     * failure define() has already emitted the named diagnostic and aborted
     * the transaction. A resumed run reached at least DEFINED, so the target is
     * already defined and this phase is skipped. */
    if (!resuming)
      {
	if (cubimport::define (iset, !dry_run) != cubimport::define_status::OK)
	  {
	    cubimport::session_close (false);
	    goto error_exit;
	  }

	/* On a normal run define committed durably, so record the DEFINED phase
	 * now - the graph snapshot below enriches this manifest with a [graph]
	 * section on success, but the phase marker is honest even if the
	 * snapshot then rejects (Q11). A --dry-run persists no manifest, so this
	 * write is skipped. */
	if (!dry_run && !cubimport::write_manifest (iset, cubimport::import_phase::DEFINED))
	  {
	    cubimport::session_close (false);
	    goto error_exit;
	  }
      }

    /* WU-21 Graph builder (snapshot): read the now-complete catalog on the
     * SAME session and build the DependencyGraph (nodes + CS-loadability +
     * constraint inventory, FK/inheritance/serial edges, cycles, level-set
     * preview). Q11: object-valued classes are reject-by-default (fail the
     * run) or, under --skip-object-classes, excluded + recorded. On failure
     * build_graph() has already emitted the named diagnostic.
     *
     * WU-50: a resumed run reads the graph back from the manifest instead. It
     * has to - the snapshot is taken while the schema is fully defined, and
     * after the Strip phase the catalog no longer carries the PK/UK/FK the
     * graph is made of, so rebuilding it from the catalog would yield a graph
     * with nothing to rebuild or re-validate. */
    {
      cubimport::dependency_graph graph;
      if (resuming && prior.has_graph)
	{
	  graph = prior.graph;
	}
      else if (cubimport::build_graph (iset, skip_object_classes, graph) != cubimport::build_graph_status::OK)
	{
	  /* A resumed run reaches here only at DEFINED-without-a-graph, where the
	   * catalog is still the fully defined one the snapshot would have read:
	   * the graph is written before the Strip phase, so a manifest that has
	   * none was killed before anything was stripped. */
	  cubimport::session_close (false);
	  goto error_exit;
	}
      cubimport::print_graph_summary (graph);

      /* Re-write the DEFINED manifest, now enriched with the graph snapshot
       * ([graph] section: counts + cycles + skipped classes). A resumed run
       * rewrites it too when it is itself still at DEFINED - there is no later
       * record to lose, and the rewrite is what gives a second kill the
       * serialized graph this one had to do without. */
      if (!dry_run && rewrite_defined && !cubimport::write_manifest (iset, cubimport::import_phase::DEFINED, &graph))
	{
	  cubimport::session_close (false);
	  goto error_exit;
	}

      /* WU-22 Stage 3 Planner (pure computation - no server calls): turn the
       * in-memory graph into a Schedule (data-phase level sets + terminal task
       * list with ordering edges) and machine-check that ordering. A machine-
       * check failure is a logic bug; build_schedule() has already emitted the
       * named diagnostic. A resumed run rebuilds the schedule too: it is pure
       * computation over the same graph, and the report + [plan] section need
       * it. */
      {
	cubimport::schedule sched;
	if (cubimport::build_schedule (graph, iset, sched) != cubimport::build_schedule_status::OK)
	  {
	    cubimport::session_close (false);
	    goto error_exit;
	  }
	cubimport::print_schedule (sched);

	/* Re-write the DEFINED manifest with the real [plan] section (data-level
	 * order + terminal task counts), replacing the WU-12 placeholder. */
	if (!dry_run && rewrite_defined
	    && !cubimport::write_manifest (iset, cubimport::import_phase::DEFINED, &graph, &sched))
	  {
	    cubimport::session_close (false);
	    goto error_exit;
	  }

	/* WU-23 --dry-run: planning is complete and the strip below is EXECUTION,
	 * not planning, so a dry run stops here - it printed exactly the plan a
	 * real run would execute and wrote no manifest. Abort so the defined
	 * schema is rolled back and the target DB is left completely unchanged. */
	if (dry_run)
	  {
	    fprintf (stdout, msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
					     IMPORTDB_MSG_DRY_RUN_COMPLETE), iset.database_name.c_str ());
	    cubimport::session_close (false);
	    return EXIT_SUCCESS;
	  }

	/* WU-50 resume guard. Exactly one phase per resumed run can be
	 * half-finished - the one right after `reached` - so the catalog is read
	 * once, here, and handed only to that phase. Later phases in this
	 * process start from a state this process itself established, and giving
	 * them a snapshot taken before their own predecessor ran would be worse
	 * than giving them none (a Strip that runs now makes a "constraint
	 * present" reading stale, and a Rebuild trusting it would skip every
	 * re-add). Not every phase needs the guard: Validate and Statistics are
	 * idempotent, Load is reconciled by emptying the tables instead, and the
	 * Trigger phase uses the server's own already-exists answer. */
	cubimport::catalog_state present;
	const bool guard_strip = resuming && prior.reached == cubimport::import_phase::DEFINED;
	const bool guard_rebuild = resuming && prior.reached == cubimport::import_phase::LOADED;
	const bool guard_fkdefine = resuming && prior.reached == cubimport::import_phase::REBUILT;
	const bool guard_triggers = resuming && prior.reached == cubimport::import_phase::STATS_UPDATED;
	const bool reload_data = resuming && prior.reached == cubimport::import_phase::STRIPPED;
	if (resuming)
	  {
	    if (!cubimport::read_catalog_state (iset.database_name, present))
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }

	    /* Before touching anything, make the target prove it is the database
	     * the manifest describes. The [set] check upstream compares a
	     * database NAME, and a resumed run skips define () - the step whose
	     * "class already exists" failure is what normally refuses a non-empty
	     * target. Without this, a manifest left in a dump directory shared by
	     * two hosts can drive a TRUNCATE over a same-named production
	     * database.
	     *
	     * The constraint half of the check applies at STRIPPED only - which is
	     * also exactly where TRUNCATE runs, so the destructive path is the
	     * guarded one. It must NOT extend to LOADED: there the next phase is
	     * the rebuild, an interrupted one legitimately leaves part of the set
	     * back, and demanding their absence would refuse the very case the
	     * rebuild guard exists for. */
	    const bool constraints_must_be_absent = (prior.reached == cubimport::import_phase::STRIPPED);
	    /* At DEFINED the strip phase is about to DROP every constraint the
	     * graph names, and the load phase will NOT truncate (that is the
	     * STRIPPED entry) - so a populated look-alike would end up stripped of
	     * its constraints with the dump's rows appended to its own. Emptiness
	     * is importdb's own invariant at this marker, so requiring it refuses
	     * no legitimate resume. */
	    const bool classes_must_be_empty = (prior.reached == cubimport::import_phase::DEFINED);
	    std::string mismatch;
	    if (!cubimport::verify_resume_target (graph, prior.stripped, constraints_must_be_absent,
						  classes_must_be_empty, present, mismatch))
	      {
		PRINT_AND_LOG_ERR_MSG (msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						       IMPORTDB_MSG_RESUME_TARGET_MISMATCH),
				       iset.database_name.c_str (), mismatch.c_str ());
		cubimport::session_close (false);
		goto error_exit;
	      }
	  }

	/* The pipeline's per-phase records. A phase a resumed run skips restores
	 * its record from the manifest instead of producing it: later phases read
	 * some of them as input (rebuild -> validate -> fkdefine), and every one
	 * of them is re-emitted into the manifest this run rewrites, so a resume
	 * past the data phase does not replace "1,200,000 rows loaded" with a
	 * zero. Its phase status is re-derived from the restored record, since
	 * that is what the exit code is made of. */
	std::vector<cubimport::stripped_constraint> stripped;
	cubimport::load_summary summary;
	cubimport::rebuild_summary rb;
	cubimport::validate_summary vs;
	cubimport::fkdefine_summary fs;
	cubimport::stats_summary ss;
	cubimport::trigger_summary ts;
	cubimport::rebuild_status rst = cubimport::rebuild_status::OK;
	cubimport::validate_status vst = cubimport::validate_status::OK;
	cubimport::fkdefine_status fst = cubimport::fkdefine_status::OK;
	cubimport::stats_status sst = cubimport::stats_status::OK;
	cubimport::trigger_status tst = cubimport::trigger_status::OK;

	/* WU-30 Strip phase (normal run only): on the still-empty defined tables,
	 * DROP every PK/UK/FK in the graph's inventory in the §4.1 global order
	 * (all FKs, then all standalone uniques, then all PKs) so the data phase
	 * (WU-31) loads into bare heaps. On failure strip () has already emitted
	 * the named diagnostic; abort the transaction and fail the run. */
	if (prior.reached < cubimport::import_phase::STRIPPED)
	  {
	    if (cubimport::strip (graph, stripped, guard_strip ? &present : NULL) != cubimport::strip_status::OK)
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }

	    /* Commit BEFORE advancing the marker (every phase below does the
	     * same). This is what makes the manifest a resume basis rather than a
	     * log of intentions: "reached X" then means X's work is durable, and
	     * a kill in the gap between the commit and the write leaves the
	     * marker one phase BEHIND the database - the safe direction, which
	     * the resume guard absorbs by re-entering a phase that turns out to
	     * be already done. The price is that a hard failure no longer rolls
	     * the target back to defined-and-empty; it leaves the completed
	     * phases in place, which is exactly what the next run resumes from. */
	    if (!cubimport::session_commit ())
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }

	    /* Record the stripped set and advance the phase marker to STRIPPED -
	     * the Gap-R resume basis and the WU-32 rebuild input. */
	    if (!cubimport::write_manifest (iset, cubimport::import_phase::STRIPPED, &graph, &sched, &stripped))
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	  }
	else
	  {
	    stripped = prior.stripped;
	  }

	/* WU-31 Load phase (normal run only): pour each table's object data into
	 * the now-bare heaps by spawning `cub_admin loaddb -C` per object file,
	 * `degree` at a time (degree 1 = serial). Skipped (object-valued) classes
	 * are not loaded. On failure load_data () has already emitted the named
	 * diagnostic (naming the failing object file); abort and fail the run. */
	if (prior.reached < cubimport::import_phase::LOADED)
	  {
	    /* WU-50: a killed data phase leaves an unknown number of committed
	     * rows - loaddb commits periodically, and the --degree > 1 children
	     * commit on their own connections - and there is no per-row resume
	     * point. The tables are bare heaps, so emptying them is cheap and
	     * makes the reload exact instead of approximately right. */
	    if (reload_data)
	      {
		int truncated = 0;
		if (!cubimport::truncate_classes (graph, truncated))
		  {
		    cubimport::session_close (false);
		    goto error_exit;
		  }
		if (!cubimport::session_commit ())
		  {
		    cubimport::session_close (false);
		    goto error_exit;
		  }
		fprintf (stdout, msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						 IMPORTDB_MSG_RESUME_TRUNCATED), truncated);
	      }

	    /* The data phase is a bounded pool of `cub_admin loaddb -C` children at
	     * every degree - serial is degree 1 - because the in-process loaddb
	     * stubs would bind this utility to the engine's libstdc++ ABI (see
	     * import_load.cpp's header). The children are independent connections
	     * and transactions, so they cannot see the caller's uncommitted strip:
	     * commit the bare heaps durable first, unconditionally. */
	    if (!cubimport::session_commit ())
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	    cubimport::load_data_status lst =
	      cubimport::load_data (iset, graph, summary, degree, user_name, password);
	    if (lst != cubimport::load_data_status::OK)
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }

	    /* Record the load summary (rows per object file + total) and advance
	     * the phase marker to LOADED. */
	    if (!cubimport::session_commit ())
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	    if (!cubimport::write_manifest (iset, cubimport::import_phase::LOADED, &graph, &sched, &stripped,
					    &summary))
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	  }
	else
	  {
	    summary = prior.load;
	  }

	/* WU-32 Rebuild phase (normal run only): re-add the stripped PK/UNIQUE
	 * (re-executing the dump's own ADD statements) and build the deferred
	 * plain indexes on the now-populated tables. A populated-table PK/UNIQUE
	 * ADD that fails on a duplicate key is left un-rebuilt, recorded in the
	 * pending-rebuild set with its parent-side FK edges withheld, and the
	 * phase continues (PARTIAL) - the successfully-rebuilt constraints and the
	 * loaded data still commit, but the run exits non-zero. A hard file error
	 * (ERR_REBUILD) aborts. */
	if (prior.reached < cubimport::import_phase::REBUILT)
	  {
	    rst = cubimport::rebuild (iset, graph, stripped, rb, guard_rebuild ? &present : NULL);
	    if (rst == cubimport::rebuild_status::ERR_REBUILD)
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }

	    /* Record the rebuild outcome (rebuilt + pending + indexes + withheld)
	     * and advance the phase marker to REBUILT. On PARTIAL the manifest is
	     * the honest record of what did and did not rebuild. */
	    if (!cubimport::session_commit ())
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	    if (!cubimport::write_manifest (iset, cubimport::import_phase::REBUILT, &graph, &sched, &stripped, &summary,
					    &rb))
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	  }
	else
	  {
	    rb = prior.rebuild;
	    /* A failed plain index makes the phase PARTIAL just as a pending
	     * PK/UNIQUE does, and it is recorded separately - deriving from
	     * `pending` alone dropped the non-zero exit an uninterrupted run
	     * would have returned. */
	    rst = (rb.pending.empty () && rb.failed_indexes.empty ()) ? cubimport::rebuild_status::OK
		  : cubimport::rebuild_status::PARTIAL;
	  }

	/* FK re-validation is no longer a phase. The engine validates while it
	 * builds the FK, so the define phase below both defines and validates, and
	 * enumerates offenders only for an edge the engine actually rejects. On a
	 * resume the prior run's per-edge outcome is restored for reporting. */
	if (prior.reached >= cubimport::import_phase::FK_DEFINED)
	  {
	    vs = prior.validate;
	  }

	/* WU-34 FK define phase (normal run only, the terminal step of the §6
	 * constraint lifecycle): define the FK on every validated-clean edge by
	 * re-executing the dump's own ADD ... FOREIGN KEY statements, bulk-building
	 * the FK b-tree on the populated tables. An edge WU-32 withheld (parent PK
	 * un-rebuilt), WU-33 found violated, or (under fail-fast) never re-validated
	 * is left undefined and recorded with its re-add DDL (manifest [fkdefine] +
	 * exceptions artifact) for an operator. All edges defined means catalog ==
	 * snapshot (the full round-trip). Any withheld edge is PARTIAL (the clean
	 * edges' FK still commits, the run exits non-zero). Only a hard error - a
	 * clean edge's FK failing unexpectedly, a source file that cannot be opened,
	 * or an exceptions-write failure - aborts (ERR_FKDEFINE). */
	if (prior.reached < cubimport::import_phase::FK_DEFINED)
	  {
	    /* The children of the load phase committed on their own connections and
	     * the rebuild is committed too, so the FK build below sees durable data
	     * and durable parent PK indexes. */
	    if (!cubimport::session_commit ())
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	    fst = cubimport::define_fks (iset, graph, rb, continue_on_error, vs, fs,
					 guard_fkdefine ? &present : NULL);
	    vst = (vs.violated_edges > 0) ? cubimport::validate_status::VIOLATED : cubimport::validate_status::OK;
	    if (fst == cubimport::fkdefine_status::ERR_FKDEFINE)
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }

	    /* Record the FK define outcome (defined edges + withheld edges with
	     * their re-add DDL) and advance the phase marker to FK_DEFINED. On a
	     * clean run this manifest records the completed round-trip (catalog ==
	     * snapshot). */
	    if (!cubimport::session_commit ())
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	    if (!cubimport::write_manifest (iset, cubimport::import_phase::FK_DEFINED, &graph, &sched, &stripped,
					    &summary, &rb, &vs, &fs))
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	  }
	else
	  {
	    fs = prior.fkdefine;
	    fst = fs.withheld.empty () ? cubimport::fkdefine_status::OK : cubimport::fkdefine_status::PARTIAL;
	  }

	/* WU-35 Statistics phase (normal run only): refresh the per-class
	 * statistics of the loaded + rebuilt tables, reusing loaddb's per-class
	 * path (sm_update_statistics with sampling) over every graph node - the
	 * Load phase deferred statistics (disable_statistics). Skipped object-valued
	 * classes are already absent from graph.nodes. Statistics are not part of
	 * the data's correctness, so a per-class failure is recorded and the phase
	 * continues; the run exits non-zero but never aborts. Recomputing them on a
	 * resume is idempotent, so this phase needs no guard either. */
	if (prior.reached < cubimport::import_phase::STATS_UPDATED)
	  {
	    sst = cubimport::update_stats (iset, graph, ss);

	    /* Record the statistics outcome (updated + failed classes) and advance
	     * the phase marker to STATS_UPDATED. */
	    if (!cubimport::session_commit ())
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	    if (!cubimport::write_manifest (iset, cubimport::import_phase::STATS_UPDATED, &graph, &sched, &stripped,
					    &summary, &rb, &vs, &fs, &ss))
	      {
		cubimport::session_close (false);
		goto error_exit;
	      }
	  }
	else
	  {
	    ss = prior.stats;
	    sst = ss.failed.empty () ? cubimport::stats_status::OK : cubimport::stats_status::PARTIAL;
	  }

	/* WU-35 Trigger define phase (normal run only, the terminal task): define
	 * the dump's deferred triggers (<prefix>_trigger) STRICTLY LAST, after the
	 * data + every constraint + the statistics are in place, so no trigger fires
	 * on the bulk-loaded rows. A trigger statement that fails is recorded and the
	 * phase continues; the run exits non-zero but never aborts (the imported data
	 * + constraints must survive). A dump with no trigger file is nothing to do.
	 * This phase always runs: a manifest that reached DONE returned above. */
	tst = cubimport::define_triggers (iset, ts, guard_triggers);

	/* Record the trigger outcome and advance the phase marker to DONE - the
	 * importdb pipeline is functionally complete. This final manifest carries the
	 * full [stats] + [triggers] record. */
	if (!cubimport::session_commit ())
	  {
	    cubimport::session_close (false);
	    goto error_exit;
	  }
	if (!cubimport::write_manifest (iset, cubimport::import_phase::DONE, &graph, &sched, &stripped,
					&summary, &rb, &vs, &fs, &ss, &ts))
	  {
	    cubimport::session_close (false);
	    goto error_exit;
	  }

	/* Normal run: commit the rebuilt + validated + FK-defined target with its
	 * refreshed statistics and defined triggers (or the honestly-recorded
	 * partial/violated result) and close the one session. A clean run ends here
	 * with catalog == snapshot, statistics present per class, and the triggers
	 * defined - the full serial import. */
	/* The trigger phase's work was already committed above, so this commit has
	 * nothing left to do - but if it somehow fails, the run must not report
	 * success on records that look clean. */
	const bool closed_clean = cubimport::session_close (true);

	/* A partial rebuild, any FK violation, any withheld FK define, a failed
	 * statistics update, or a failed trigger define is a committed-but-incomplete
	 * result: exit non-zero so downstream tooling sees the manifest's
	 * pending-rebuild / [validate] / [fkdefine] / [stats] / [triggers] records and
	 * the exceptions artifact an operator repairs from. A resumed run re-derives
	 * these from the records it restored, so the verdict of a resumed import is
	 * the verdict the whole import earned, not just its last leg. */
	if (!closed_clean || rst == cubimport::rebuild_status::PARTIAL || vst == cubimport::validate_status::VIOLATED
	    || fst == cubimport::fkdefine_status::PARTIAL || sst == cubimport::stats_status::PARTIAL
	    || tst == cubimport::trigger_status::PARTIAL)
	  {
	    exit_code = EXIT_FAILURE;
	  }

	/* WU-53: the run was allowed onto an HA master with --allow-ha. Say plainly,
	 * at the end where an operator reads the verdict, that the standby did not
	 * get the rows -- the schema replicated and the data did not, and nothing
	 * else in this output would reveal that. */
	if (target_is_ha)
	  {
	    PRINT_AND_LOG_ERR_MSG (msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
						   IMPORTDB_MSG_HA_SLAVE_DIVERGED), iset.database_name.c_str ());
	  }

	/* WU-36 Reporter v1: print the consolidated RunReport (10-design.md §7) -
	 * planned data order, per-class done/pending/skipped state, loaded rows, and
	 * the pending-rebuild + withheld-FK repair records with their re-add DDL - so
	 * an operator can answer the run's outcome (done/pending/skipped + error
	 * counts) from the output alone (G5). A pure pass over the in-memory summaries;
	 * the dry-run and hard-error paths return before here and print their own
	 * terminal message. */
	cubimport::print_report (iset, graph, sched, summary, rb, vs, fs, ss, ts);
      }
    }
  }

  return exit_code;

print_import_usage:
  fprintf (stderr, msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB, IMPORTDB_MSG_USAGE),
	   basename (arg->argv0));
  util_log_write_errid (MSGCAT_UTIL_GENERIC_INVALID_ARGUMENT);

error_exit:
  /* G5's last clause - "an operator can determine, from importdb output alone,
   * ... a resume point" - and the one place it can be honoured. A run that is
   * KILLED never reaches the Reporter, and a run that completes has no resume
   * point to name, so the only path that both fails and can still speak is this
   * one. Since WU-50 each phase commits before its marker advances, so a hard
   * error no longer leaves the target defined-and-empty: it leaves the completed
   * phases in place, and an operator who is not told that will reasonably assume
   * the database is untouched. Re-read the manifest rather than tracking the
   * phase through the pipeline's nested scopes - it is one small file, and it is
   * the same record the next run will resume from, so it cannot disagree. */
  if (!dry_run && dump_dir != NULL)
    {
      cubimport::manifest_state left;
      std::string ignored;
      if (cubimport::read_manifest (dump_dir, left, ignored) == cubimport::manifest_read::OK && left.has_phase
	  && left.reached > cubimport::import_phase::DISCOVERED)
	{
	  const std::string path = std::string (dump_dir) + "/importdb.manifest";
	  fprintf (stdout, msgcat_message (MSGCAT_CATALOG_UTILS, MSGCAT_UTIL_SET_IMPORTDB,
					   IMPORTDB_MSG_PARTIALLY_IMPORTED), database_name,
		   cubimport::phase_name (left.reached), path.c_str ());
	}
    }

  return EXIT_FAILURE;
}
