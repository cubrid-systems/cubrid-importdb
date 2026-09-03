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
 * import_ddl_text.cpp - pure string functions over an unloaddb schema file. The
 * header carries the contract; the only note the bodies need is that both the
 * statement split and the name match assume the dump puts no ';' or bracketed
 * identifier inside a string literal, which unloaddb's output does not.
 */

#include "import_ddl_text.hpp"

#include <cctype>

namespace cubimport
{

  bool
  ends_with (const std::string &name, const std::string &suffix)
  {
    return name.size () >= suffix.size () && name.compare (name.size () - suffix.size (), suffix.size (), suffix) == 0;
  }

  std::string
  to_upper (const std::string &s)
  {
    std::string u = s;
    for (char &c : u)
      {
	c = (char) toupper ((unsigned char) c);
      }
    return u;
  }

  std::string
  trim (const std::string &s)
  {
    const char *ws = " \t\r\n";
    size_t b = s.find_first_not_of (ws);
    if (b == std::string::npos)
      {
	return "";
      }
    size_t e = s.find_last_not_of (ws);
    return s.substr (b, e - b + 1);
  }

  std::vector<std::string>
  split_statements (const std::string &text)
  {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size ())
      {
	size_t semi = text.find (';', start);
	std::string stmt = (semi == std::string::npos) ? text.substr (start) : text.substr (start, semi - start);
	std::string t = trim (stmt);
	if (!t.empty ())
	  {
	    out.push_back (t);
	  }
	if (semi == std::string::npos)
	  {
	    break;
	  }
	start = semi + 1;
      }
    return out;
  }

  std::string
  statement_class (const std::string &stmt)
  {
    /* Scanned forward with bracket depth rather than searched for: a delimited
     * class name may itself contain " ADD ", and a plain find () would take that
     * one and walk back to the owner bracket instead of the class. */
    const std::string upper = to_upper (stmt);
    size_t close = std::string::npos;
    bool bracketed = false;
    bool at_add = false;
    for (size_t i = 0; i < stmt.size (); i++)
      {
	if (stmt[i] == '[')
	  {
	    bracketed = true;
	  }
	else if (stmt[i] == ']')
	  {
	    bracketed = false;
	    close = i;
	  }
	else if (!bracketed && upper.compare (i, 5, " ADD ") == 0)
	  {
	    at_add = true;
	    break;
	  }
      }
    if (!at_add || close == std::string::npos)
      {
	return std::string ();
      }
    const size_t open = stmt.rfind ('[', close);
    return open == std::string::npos ? std::string () : stmt.substr (open + 1, close - open - 1);
  }

  std::string
  constraint_key (const std::string &cls, const std::string &name)
  {
    return cls + "\t" + name;
  }

  std::vector<std::string>
  constraint_names (const std::string &stmt)
  {
    std::vector<std::string> names;
    const std::string upper = to_upper (stmt);
    const std::string kw = "CONSTRAINT";
    size_t pos = 0;
    while ((pos = upper.find (kw, pos)) != std::string::npos)
      {
	size_t lb = stmt.find ('[', pos + kw.size ());
	if (lb == std::string::npos)
	  {
	    break;
	  }
	size_t rb = stmt.find (']', lb + 1);
	if (rb == std::string::npos)
	  {
	    break;
	  }
	names.push_back (stmt.substr (lb + 1, rb - lb - 1));
	pos = rb + 1;
      }
    return names;
  }

} // namespace cubimport
