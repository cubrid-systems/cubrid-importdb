#!/usr/bin/env python3
"""Regenerate src/importdb_messages.cpp from the engine's utils.msg set 61.

In-tree these strings are compiled into $CUBRID/msg/<locale>/utils.cat. A
separate repo cannot write there, so it carries the table and the build renames
the lookup (-Dmsgcat_message=importdb_msgcat_message).

Usage: gen_messages.py <engine-source-dir> [locale] > src/importdb_messages.cpp
"""
import re, sys

src = sys.argv[1]
locale = sys.argv[2] if len(sys.argv) > 2 else 'en_US.utf8'
cat = open(f'{src}/msg/{locale}/utils.msg', encoding='utf-8').read()
k = cat.index('$set 61')
rest = cat[k + 8:]
m = re.search(r'\n\$set \d+', rest)
block = rest[:m.start()] if m else rest

# msgcat continues a message with a trailing backslash; fold those first
folded = re.sub(r'\\\n', '', block)
msgs = {}
for line in folded.split('\n'):
    mm = re.match(r'^(\d+)\s(.*)$', line)
    if mm:
        msgs[int(mm.group(1))] = mm.group(2)
if not msgs:
    sys.exit('no messages parsed from set 61')

def cstr(s):
    return '"' + s.replace('"', '\\"') + '"'

print('''/*
 * importdb_messages.cpp -- the utility's own message table. GENERATED.
 *
 * Regenerate with tools/gen_messages.py <engine-source-dir>.
 */
#include <cstddef>

extern "C" const char *importdb_msgcat_message (int cat_id, int set_id, int msg_id);

namespace
{
  struct entry { int id; const char *text; };

  const entry importdb_messages[] = {''')
for i in sorted(msgs):
    print('    { %d, %s },' % (i, cstr(msgs[i])))
print('''  };
}

extern "C" const char *
importdb_msgcat_message (int cat_id, int set_id, int msg_id)
{
  (void) cat_id;
  (void) set_id;
  for (size_t i = 0; i < sizeof (importdb_messages) / sizeof (importdb_messages[0]); i++)
    {
      if (importdb_messages[i].id == msg_id)
        {
          return importdb_messages[i].text;
        }
    }
  return "importdb: message not found in the utility's own catalog\\n";
}''')
