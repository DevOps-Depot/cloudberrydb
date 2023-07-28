/*
 *	version.c
 *
 *	Postgres-version-specific routines
 *
 * 	Portions Copyright (c) 2023, HashData Technology Limited.
 *	Copyright (c) 2010-2021, PostgreSQL Global Development Group
 *	src/bin/pg_upgrade/version.c
 */

#include "postgres_fe.h"

#include "catalog/pg_class_d.h"
#include "fe_utils/string_utils.h"
#include "pg_upgrade.h"

/*
 * new_9_0_populate_pg_largeobject_metadata()
 *	new >= 9.0, old <= 8.4
 *	9.0 has a new pg_largeobject permission table
 */
void
new_9_0_populate_pg_largeobject_metadata(ClusterInfo *cluster, bool check_mode)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char		output_path[MAXPGPATH];

	prep_status("Checking for large objects");

	snprintf(output_path, sizeof(output_path), "pg_largeobject.sql");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		int			i_count;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* find if there are any large objects */
		res = executeQueryOrDie(conn,
								"SELECT count(*) "
								"FROM	pg_catalog.pg_largeobject ");

		i_count = PQfnumber(res, "count");
		if (atoi(PQgetvalue(res, 0, i_count)) != 0)
		{
			found = true;
			if (!check_mode)
			{
				PQExpBufferData connectbuf;

				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %s\n", output_path,
							 strerror(errno));

				initPQExpBuffer(&connectbuf);
				appendPsqlMetaConnect(&connectbuf, active_db->db_name);
				fputs(connectbuf.data, script);
				termPQExpBuffer(&connectbuf);

				fprintf(script,
						"SELECT pg_catalog.lo_create(t.loid)\n"
						"FROM (SELECT DISTINCT loid FROM pg_catalog.pg_largeobject) AS t;\n");
			}
		}

		PQclear(res);
		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_WARNING, "warning");
		if (check_mode)
			pg_log(PG_WARNING, "\n"
				   "Your installation contains large objects.  The new database has an\n"
				   "additional large object permission table.  After upgrading, you will be\n"
				   "given a command to populate the pg_largeobject_metadata table with\n"
				   "default permissions.\n\n");
		else
			pg_log(PG_WARNING, "\n"
				   "Your installation contains large objects.  The new database has an\n"
				   "additional large object permission table, so default permissions must be\n"
				   "defined for all large objects.  The file\n"
				   "    %s\n"
				   "when executed by psql by the database superuser will set the default\n"
				   "permissions.\n\n",
				   output_path);
	}
	else
		check_ok();
}

static PQExpBuffer
create_recursive_oids(PGconn *conn, int major_version, const char *base_query)
{
	PQExpBuffer querybuf;
	PQExpBufferData template;
	PGresult   *res;
	int iter_no;
	int n;

	querybuf = createPQExpBuffer();
	appendPQExpBuffer(querybuf,
			"WITH oids_0 AS ( %s ) ", base_query);
	initPQExpBuffer(&template);
	appendPQExpBufferStr(&template,
			", oids_%d AS ( "
			"WITH x AS ( "
			"	SELECT oid FROM oids_%d"
			") "
/* domains on any type selected so far */
			"	SELECT t.oid FROM pg_catalog.pg_type t, x WHERE typbasetype = x.oid AND typtype = 'd' "
			"	UNION ALL "
/* arrays over any type selected so far */
			"	SELECT t.oid FROM pg_catalog.pg_type t, x WHERE typelem = x.oid AND typtype = 'b' "
			"	UNION ALL "
/* composite types containing any type selected so far */
			"	SELECT t.oid FROM pg_catalog.pg_type t, pg_catalog.pg_class c, pg_catalog.pg_attribute a, x "
			"	WHERE t.typtype = 'c' AND "
			"		  t.oid = c.reltype AND "
			"		  c.oid = a.attrelid AND "
			"		  NOT a.attisdropped AND "
			"		  a.atttypid = x.oid "
			);
	if (GET_MAJOR_VERSION(major_version) >= 902)
		appendPQExpBufferStr(&template,
							 "		UNION ALL "
		/* ranges containing any type selected so far */
							 "		SELECT t.oid FROM pg_catalog.pg_type t, pg_catalog.pg_range r, x "
							 "		WHERE t.typtype = 'r' AND r.rngtypid = t.oid AND r.rngsubtype = x.oid");
	appendPQExpBufferStr(&template, ") ");

	iter_no = 0;
	res = executeQueryOrDie(conn,
					"%s SELECT count(1) FROM oids_%d; ",
					querybuf->data, iter_no);
	Assert(PQntuples(res) == 1);
	Assert(PQnfields(res) == 1);
	n = strtoul(PQgetvalue(res, 0, 0), NULL, 10);

	while (n > 0)
	{
		iter_no++;
		appendPQExpBuffer(querybuf, template.data, iter_no, iter_no - 1);
		res = executeQueryOrDie(conn,
					"%s SELECT count(1) FROM oids_%d; ",
					querybuf->data, iter_no);
		Assert(PQntuples(res) == 1);
		Assert(PQnfields(res) == 1);
		n = strtoul(PQgetvalue(res, 0, 0), NULL, 10);
	}

	/* Add footer, merge all oids_<d> into one CTE table */
	appendPQExpBufferStr(querybuf,
				" , oids(oid) AS ( "
				"SELECT oid FROM oids_0 ");
	for (n = 1; n <= iter_no; n++)
	{
		appendPQExpBuffer(querybuf,
				"UNION ALL "
				"SELECT oid FROM oids_%d ",
				n);
	}
	appendPQExpBufferStr(querybuf, ") ");
	termPQExpBuffer(&template);

	return querybuf;
}
/*
 * check_for_data_types_usage()
 *	Detect whether there are any stored columns depending on given type(s)
 *
 * If so, write a report to the given file name, and return true.
 *
 * base_query should be a SELECT yielding a single column named "oid",
 * containing the pg_type OIDs of one or more types that are known to have
 * inconsistent on-disk representations across server versions.
 *
 * We check for the type(s) in tables, matviews, and indexes, but not views;
 * there's no storage involved in a view.
 */
bool
check_for_data_types_usage(ClusterInfo *cluster,
						   const char *base_query,
						   const char *output_path)
{
	bool		found = false;
	FILE	   *script = NULL;
	int			dbnum;

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PQExpBuffer cte_oids;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);
		PQExpBufferData querybuf;
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname,
					i_attname;

		cte_oids = create_recursive_oids(conn, cluster->major_version, base_query);
		/*
		 * The type(s) of interest might be wrapped in a domain, array,
		 * composite, or range, and these container types can be nested (to
		 * varying extents depending on server version, but that's not of
		 * concern here).  To handle all these cases we need a recursive CTE.
		 */
		initPQExpBuffer(&querybuf);
#if 0
		/* Move this recursive CTE to function create_recursive_oids().
		 * GPDB doesn't support recursive CTE that the subquery in recursive
		 * part has self-reference. See 3f5cf5c730f2f32d524d03c193743c70e
		 *
		 * The return WITH clause contains all expaneded sub-with clauses.
		 */
		appendPQExpBuffer(&querybuf,
						  "WITH RECURSIVE oids AS ( "
		/* start with the type(s) returned by base_query */
						  "	%s "
						  "	UNION ALL "
						  "	SELECT * FROM ( "
		/* inner WITH because we can only reference the CTE once */
						  "		WITH x AS (SELECT oid FROM oids) "
		/* domains on any type selected so far */
						  "			SELECT t.oid FROM pg_catalog.pg_type t, x WHERE typbasetype = x.oid AND typtype = 'd' "
						  "			UNION ALL "
		/* arrays over any type selected so far */
						  "			SELECT t.oid FROM pg_catalog.pg_type t, x WHERE typelem = x.oid AND typtype = 'b' "
						  "			UNION ALL "
		/* composite types containing any type selected so far */
						  "			SELECT t.oid FROM pg_catalog.pg_type t, pg_catalog.pg_class c, pg_catalog.pg_attribute a, x "
						  "			WHERE t.typtype = 'c' AND "
						  "				  t.oid = c.reltype AND "
						  "				  c.oid = a.attrelid AND "
						  "				  NOT a.attisdropped AND "
						  "				  a.atttypid = x.oid ",
						  base_query);

		/* Ranges were introduced in 9.2 */
		if (GET_MAJOR_VERSION(cluster->major_version) >= 902)
			appendPQExpBufferStr(&querybuf,
								 "			UNION ALL "
			/* ranges containing any type selected so far */
								 "			SELECT t.oid FROM pg_catalog.pg_type t, pg_catalog.pg_range r, x "
								 "			WHERE t.typtype = 'r' AND r.rngtypid = t.oid AND r.rngsubtype = x.oid");

		appendPQExpBufferStr(&querybuf,
							 "	) foo "
							 ") "
#endif
		appendPQExpBuffer(&querybuf,
							 "%s "
		/* now look for stored columns of any such type */
							 "SELECT n.nspname, c.relname, a.attname "
							 "FROM	pg_catalog.pg_class c, "
							 "		pg_catalog.pg_namespace n, "
							 "		pg_catalog.pg_attribute a "
							 "WHERE	c.oid = a.attrelid AND "
							 "		NOT a.attisdropped AND "
							 "		a.atttypid IN (SELECT oid FROM oids) AND "
							 "		c.relkind IN ("
							 CppAsString2(RELKIND_RELATION) ", "
							 CppAsString2(RELKIND_MATVIEW) ", "
							 CppAsString2(RELKIND_INDEX) ") AND "
							 "		c.relnamespace = n.oid AND "
		/* exclude possible orphaned temp tables */
							 "		n.nspname !~ '^pg_temp_' AND "
							 "		n.nspname !~ '^pg_toast_temp_' AND "
		/* exclude system catalogs, too */
							 "		n.nspname NOT IN ('pg_catalog', 'information_schema')",
			cte_oids->data);

		res = executeQueryOrDie(conn, "%s", querybuf.data);
		destroyPQExpBuffer(cte_oids);

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		i_attname = PQfnumber(res, "attname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n", output_path,
						 strerror(errno));
			if (!db_used)
			{
				fprintf(script, "In database: %s\n", active_db->db_name);
				db_used = true;
			}
			fprintf(script, "  %s.%s.%s\n",
					PQgetvalue(res, rowno, i_nspname),
					PQgetvalue(res, rowno, i_relname),
					PQgetvalue(res, rowno, i_attname));
		}

		PQclear(res);

		termPQExpBuffer(&querybuf);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	return found;
}

/*
 * check_for_data_type_usage()
 *	Detect whether there are any stored columns depending on the given type
 *
 * If so, write a report to the given file name, and return true.
 *
 * type_name should be a fully qualified type name.  This is just a
 * trivial wrapper around check_for_data_types_usage() to convert a
 * type name into a base query.
 */
bool
check_for_data_type_usage(ClusterInfo *cluster,
						  const char *type_name,
						  const char *output_path)
{
	bool		found;
	char	   *base_query;

	base_query = psprintf("SELECT '%s'::pg_catalog.regtype AS oid",
						  type_name);

	found = check_for_data_types_usage(cluster, base_query, output_path);

	free(base_query);

	return found;
}


/*
 * old_9_3_check_for_line_data_type_usage()
 *	9.3 -> 9.4
 *	Fully implement the 'line' data type in 9.4, which previously returned
 *	"not enabled" by default and was only functionally enabled with a
 *	compile-time switch; as of 9.4 "line" has a different on-disk
 *	representation format.
 */
void
old_9_3_check_for_line_data_type_usage(ClusterInfo *cluster)
{
	char		output_path[MAXPGPATH];

	prep_status("Checking for incompatible \"line\" data type");

	snprintf(output_path, sizeof(output_path), "tables_using_line.txt");

	if (check_for_data_type_usage(cluster, "pg_catalog.line", output_path))
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains the \"line\" data type in user tables.\n"
				 "This data type changed its internal and input/output format\n"
				 "between your old and new versions so this\n"
				 "cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 * old_9_6_check_for_unknown_data_type_usage()
 *	9.6 -> 10
 *	It's no longer allowed to create tables or views with "unknown"-type
 *	columns.  We do not complain about views with such columns, because
 *	they should get silently converted to "text" columns during the DDL
 *	dump and reload; it seems unlikely to be worth making users do that
 *	by hand.  However, if there's a table with such a column, the DDL
 *	reload will fail, so we should pre-detect that rather than failing
 *	mid-upgrade.  Worse, if there's a matview with such a column, the
 *	DDL reload will silently change it to "text" which won't match the
 *	on-disk storage (which is like "cstring").  So we *must* reject that.
 */
void
old_9_6_check_for_unknown_data_type_usage(ClusterInfo *cluster)
{
	char		output_path[MAXPGPATH];

	prep_status("Checking for invalid \"unknown\" user columns");

	snprintf(output_path, sizeof(output_path), "tables_using_unknown.txt");

	if (check_for_data_type_usage(cluster, "pg_catalog.unknown", output_path))
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains the \"unknown\" data type in user tables.\n"
				 "This data type is no longer allowed in tables, so this\n"
				 "cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}

/*
 * old_9_6_invalidate_hash_indexes()
 *	9.6 -> 10
 *	Hash index binary format has changed from 9.6->10.0
 */
void
old_9_6_invalidate_hash_indexes(ClusterInfo *cluster, bool check_mode)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char	   *output_path = "reindex_hash.sql";

	prep_status("Checking for hash indexes");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_nspname,
					i_relname;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* find hash indexes */
		res = executeQueryOrDie(conn,
								"SELECT n.nspname, c.relname "
								"FROM	pg_catalog.pg_class c, "
								"		pg_catalog.pg_index i, "
								"		pg_catalog.pg_am a, "
								"		pg_catalog.pg_namespace n "
								"WHERE	i.indexrelid = c.oid AND "
								"		c.relam = a.oid AND "
								"		c.relnamespace = n.oid AND "
								"		a.amname = 'hash'"
			);

		ntups = PQntuples(res);
		i_nspname = PQfnumber(res, "nspname");
		i_relname = PQfnumber(res, "relname");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;
			if (!check_mode)
			{
				if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
					pg_fatal("could not open file \"%s\": %s\n", output_path,
							 strerror(errno));
				if (!db_used)
				{
					PQExpBufferData connectbuf;

					initPQExpBuffer(&connectbuf);
					appendPsqlMetaConnect(&connectbuf, active_db->db_name);
					fputs(connectbuf.data, script);
					termPQExpBuffer(&connectbuf);
					db_used = true;
				}
				fprintf(script, "REINDEX INDEX %s.%s;\n",
						quote_identifier(PQgetvalue(res, rowno, i_nspname)),
						quote_identifier(PQgetvalue(res, rowno, i_relname)));
			}
		}

		PQclear(res);

		if (!check_mode && db_used)
		{
			/* mark hash indexes as invalid */
			PQclear(executeQueryOrDie(conn,
									  "UPDATE pg_catalog.pg_index i "
									  "SET	indisvalid = false "
									  "FROM	pg_catalog.pg_class c, "
									  "		pg_catalog.pg_am a, "
									  "		pg_catalog.pg_namespace n "
									  "WHERE	i.indexrelid = c.oid AND "
									  "		c.relam = a.oid AND "
									  "		c.relnamespace = n.oid AND "
									  "		a.amname = 'hash'"));
		}

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_WARNING, "warning");
		if (check_mode)
			pg_log(PG_WARNING, "\n"
				   "Your installation contains hash indexes.  These indexes have different\n"
				   "internal formats between your old and new clusters, so they must be\n"
				   "reindexed with the REINDEX command.  After upgrading, you will be given\n"
				   "REINDEX instructions.\n\n");
		else
			pg_log(PG_WARNING, "\n"
				   "Your installation contains hash indexes.  These indexes have different\n"
				   "internal formats between your old and new clusters, so they must be\n"
				   "reindexed with the REINDEX command.  The file\n"
				   "    %s\n"
				   "when executed by psql by the database superuser will recreate all invalid\n"
				   "indexes; until then, none of these indexes will be used.\n\n",
				   output_path);
	}
	else
		check_ok();
}

/*
 * old_11_check_for_sql_identifier_data_type_usage()
 *	11 -> 12
 *	In 12, the sql_identifier data type was switched from name to varchar,
 *	which does affect the storage (name is by-ref, but not varlena). This
 *	means user tables using sql_identifier for columns are broken because
 *	the on-disk format is different.
 */
void
old_11_check_for_sql_identifier_data_type_usage(ClusterInfo *cluster)
{
	char		output_path[MAXPGPATH];

	prep_status("Checking for invalid \"sql_identifier\" user columns");

	snprintf(output_path, sizeof(output_path), "tables_using_sql_identifier.txt");

	if (check_for_data_type_usage(cluster, "information_schema.sql_identifier",
								  output_path))
	{
		pg_log(PG_REPORT, "fatal\n");
		pg_fatal("Your installation contains the \"sql_identifier\" data type in user tables.\n"
				 "The on-disk format for this data type has changed, so this\n"
				 "cluster cannot currently be upgraded.  You can\n"
				 "drop the problem columns and restart the upgrade.\n"
				 "A list of the problem columns is in the file:\n"
				 "    %s\n\n", output_path);
	}
	else
		check_ok();
}


/*
 * report_extension_updates()
 *	Report extensions that should be updated.
 */
void
report_extension_updates(ClusterInfo *cluster)
{
	int			dbnum;
	FILE	   *script = NULL;
	bool		found = false;
	char	   *output_path = "update_extensions.sql";

	prep_status("Checking for extension updates");

	for (dbnum = 0; dbnum < cluster->dbarr.ndbs; dbnum++)
	{
		PGresult   *res;
		bool		db_used = false;
		int			ntups;
		int			rowno;
		int			i_name;
		DbInfo	   *active_db = &cluster->dbarr.dbs[dbnum];
		PGconn	   *conn = connectToServer(cluster, active_db->db_name);

		/* find extensions needing updates */
		res = executeQueryOrDie(conn,
								"SELECT name "
								"FROM pg_available_extensions "
								"WHERE installed_version != default_version"
			);

		ntups = PQntuples(res);
		i_name = PQfnumber(res, "name");
		for (rowno = 0; rowno < ntups; rowno++)
		{
			found = true;

			if (script == NULL && (script = fopen_priv(output_path, "w")) == NULL)
				pg_fatal("could not open file \"%s\": %s\n", output_path,
						 strerror(errno));
			if (!db_used)
			{
				PQExpBufferData connectbuf;

				initPQExpBuffer(&connectbuf);
				appendPsqlMetaConnect(&connectbuf, active_db->db_name);
				fputs(connectbuf.data, script);
				termPQExpBuffer(&connectbuf);
				db_used = true;
			}
			fprintf(script, "ALTER EXTENSION %s UPDATE;\n",
					quote_identifier(PQgetvalue(res, rowno, i_name)));
		}

		PQclear(res);

		PQfinish(conn);
	}

	if (script)
		fclose(script);

	if (found)
	{
		report_status(PG_REPORT, "notice");
		pg_log(PG_REPORT, "\n"
			   "Your installation contains extensions that should be updated\n"
			   "with the ALTER EXTENSION command.  The file\n"
			   "    %s\n"
			   "when executed by psql by the database superuser will update\n"
			   "these extensions.\n\n",
			   output_path);
	}
	else
		check_ok();
}
