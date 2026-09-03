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
 * import_ddl_text.hpp - reading a dump's DDL as text
 *
 * The Rebuild and FK-define phases both re-execute statements the dump already
 * carries, and both must first decide which statements those are: split a schema
 * file into statements, then match each statement's class and constraint names
 * against a set the Strip phase or the graph recorded. That matching is text over
 * another tool's output, so it lives in one place -- a statement shape one phase
 * learns to read is a shape both read, and a gap in the matching is silent by
 * nature: an unmatched statement is simply not executed.
 *
 * Nothing here touches the server or the message catalog; these are pure string
 * functions over the contents of an unloaddb schema file.
 */

#ifndef _IMPORT_DDL_TEXT_HPP_
#define _IMPORT_DDL_TEXT_HPP_

#include <string>
#include <vector>

namespace cubimport
{

  /* True when s ends with suffix. */
  bool ends_with (const std::string &s, const std::string &suffix);

  std::string to_upper (const std::string &s);

  /* s without leading or trailing space, tab, CR or LF. */
  std::string trim (const std::string &s);

  /* The file's statements, split on ';' and trimmed, empties dropped. The dump's
   * DDL puts no ';' inside a literal, which is what makes this safe. */
  std::vector<std::string> split_statements (const std::string &text);

  /* The class an ALTER statement targets -- the bracketed identifier before its ADD
   * keyword, owner-qualified or not. Empty when the shape does not match, which
   * matches no recorded constraint: a caller must reconcile what it matched against
   * what it was given rather than read an unmatched statement as nothing to do. */
  std::string statement_class (const std::string &stmt);

  /* A constraint is identified by its class AND its name: CUBRID constraint names
   * are unique per class, not per database, so two classes may each carry a [u1]
   * and two children each an [fk1]. */
  std::string constraint_key (const std::string &cls, const std::string &name);

  /* Every constraint name in a statement's "CONSTRAINT [<name>]" clauses
   * (case-insensitive on the keyword; the bracketed name keeps its case). One
   * ALTER CLASS statement may add a PK and a standalone UNIQUE together, so this
   * can return more than one; an FK ADD statement carries exactly one. */
  std::vector<std::string> constraint_names (const std::string &stmt);

} // namespace cubimport

#endif /* _IMPORT_DDL_TEXT_HPP_ */
