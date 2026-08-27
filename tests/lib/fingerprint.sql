-- fingerprint.sql -- the catalog half of the round-trip claim.
--
-- Run with `csql -t -N -i` against the source and against the import; the two
-- outputs must be byte-identical. Every row is tagged so a diff names the kind
-- of object that moved. created_time / updated_time are deliberately absent:
-- they differ between a source and its import by construction.
--
-- Ordering is by the tagged columns, so ORDER BY positions are shifted by one
-- past the tag literal.

SELECT '#CLASS', class_name, class_type, partitioned, is_reuse_oid_class, collation
  FROM db_class
 WHERE is_system_class = 'NO'
 ORDER BY 2;

SELECT '#ATTR', c.class_name, a.attr_name, a.attr_type, a.def_order, a.from_class_name,
       a.data_type, a.prec, a.scale, a.charset, a.collation, a.domain_class_name,
       a.default_value, a.is_partition_key, a.is_nullable
  FROM db_attribute a, db_class c
 WHERE a.class_name = c.class_name AND a.owner_name = c.owner_name AND c.is_system_class = 'NO'
 ORDER BY 2, 5, 3;

SELECT '#IDX', i.class_name, i.index_name, i.is_unique, i.is_reverse, i.key_count,
       i.is_primary_key, i.is_foreign_key, i.filter_expression, i.have_function, i.status,
       i.referential_index_class_name, i.referential_index_name, i.delete_rule, i.update_rule,
       i.index_type
  FROM db_index i, db_class c
 WHERE i.class_name = c.class_name AND i.owner_name = c.owner_name AND c.is_system_class = 'NO'
 ORDER BY 2, 3;

SELECT '#IKEY', k.class_name, k.index_name, k.key_attr_name, k.key_order, k.asc_desc,
       k.key_prefix_length, k.func
  FROM db_index_key k, db_class c
 WHERE k.class_name = c.class_name AND k.owner_name = c.owner_name AND c.is_system_class = 'NO'
 ORDER BY 2, 3, 5;

SELECT '#SUPER', class_name, super_class_name
  FROM db_direct_super_class
 ORDER BY 2, 3;

SELECT '#PART', class_name, partition_name, partition_class_name, partition_type,
       partition_expr, partition_values
  FROM db_partition
 ORDER BY 2, 3;

-- start_val is deliberately absent. unloaddb writes a serial's CURRENT value
-- as its START WITH, so an imported serial's start_val equals the source's
-- current_val. That is an unloaddb property, not an importdb one, and the
-- roundtrip case asserts it separately from the dump text.
SELECT '#SERIAL', name, increment_val, max_val, min_val, cyclic, cached_num,
       class_name, attr_name, current_val
  FROM db_serial
 ORDER BY 2;

SELECT '#TRIGGER', trigger_name, target_class_name, target_attr_name, action_type, action_time
  FROM db_trigger
 ORDER BY 2;
