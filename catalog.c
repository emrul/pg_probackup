/*-------------------------------------------------------------------------
 *
 * catalog.c: backup catalog opration
 *
 * Copyright (c) 2009-2011, NIPPON TELEGRAPH AND TELEPHONE CORPORATION
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#include <dirent.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "pgut/pgut-port.h"

static pgBackup *catalog_read_ini(const char *path);

#define BOOL_TO_STR(val)	((val) ? "true" : "false")

static int lock_fd = -1;

/*
 * Lock of the catalog with pg_probackup.conf file and return 0.
 * If the lock is held by another one, return 1 immediately.
 */
int
catalog_lock(void)
{
	int		ret;
	char	id_path[MAXPGPATH];

	join_path_components(id_path, backup_path, PG_RMAN_INI_FILE);
	lock_fd = open(id_path, O_RDWR);
	if (lock_fd == -1)
		elog(errno == ENOENT ? ERROR : ERROR,
			"cannot open file \"%s\": %s", id_path, strerror(errno));

#ifdef __IBMC__
	ret = lockf(lock_fd, LOCK_EX | LOCK_NB, 0);	/* non-blocking */
#else
	ret = flock(lock_fd, LOCK_EX | LOCK_NB);	/* non-blocking */
#endif
	if (ret == -1)
	{
		if (errno == EWOULDBLOCK)
		{
			close(lock_fd);
			return 1;
		}
		else
		{
			int errno_tmp = errno;
			close(lock_fd);
			elog(ERROR, "cannot lock file \"%s\": %s", id_path,
				strerror(errno_tmp));
		}
	}

	return 0;
}

/*
 * Release catalog lock.
 */
void
catalog_unlock(void)
{
	close(lock_fd);
	lock_fd = -1;
}

/*
 * Create a pgBackup which taken at timestamp.
 * If no backup matches, return NULL.
 */
pgBackup *
catalog_get_backup(time_t timestamp)
{
	pgBackup	tmp;
	char		ini_path[MAXPGPATH];

	tmp.start_time = timestamp;
	pgBackupGetPath(&tmp, ini_path, lengthof(ini_path), BACKUP_INI_FILE);

	return catalog_read_ini(ini_path);
}

static bool
IsDir(const char *dirpath, const char *entry)
{
	char		path[MAXPGPATH];
	struct stat	st;

	snprintf(path, MAXPGPATH, "%s/%s", dirpath, entry);

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * Create list fo backups started between begin and end from backup catalog.
 * If range was NULL, all of backup are listed.
 * The list is sorted in order of descending start time.
 */
parray *
catalog_get_backup_list(time_t backup_id)
{
	DIR			   *date_dir = NULL;
	struct dirent  *date_ent = NULL;
	DIR			   *time_dir = NULL;
	char			backups_path[MAXPGPATH];
	char			date_path[MAXPGPATH];
	parray		   *backups = NULL;
	pgBackup	   *backup = NULL;

	/* open backup root directory */
	join_path_components(backups_path, backup_path, BACKUPS_DIR);
	date_dir = opendir(backups_path);
	if (date_dir == NULL)
	{
		elog(WARNING, "cannot open directory \"%s\": %s", backups_path,
			strerror(errno));
		goto err_proc;
	}

	/* scan date/time directories and list backups in the range */
	backups = parray_new();
	for (; (date_ent = readdir(date_dir)) != NULL; errno = 0)
	{
		char ini_path[MAXPGPATH];

		/* skip not-directory entries and hidden entries */
		if (!IsDir(backups_path, date_ent->d_name) || date_ent->d_name[0] == '.')
			continue;

		/* open subdirectory (date directory) and search time directory */
		join_path_components(date_path, backups_path, date_ent->d_name);

		/* read backup information from backup.ini */
		snprintf(ini_path, MAXPGPATH, "%s/%s", date_path, BACKUP_INI_FILE);
		backup = catalog_read_ini(ini_path);

		/* ignore corrupted backup */
		if (backup)
		{
			if (backup_id != 0 && backup_id != backup->start_time)
			{
				pgBackupFree(backup);
				continue;
			}
			parray_append(backups, backup);
			backup = NULL;
		}
		if (errno && errno != ENOENT)
		{
			elog(WARNING, "cannot read date directory \"%s\": %s",
				date_ent->d_name, strerror(errno));
			goto err_proc;
		}
	}
	if (errno)
	{
		elog(WARNING, "cannot read backup root directory \"%s\": %s",
			backups_path, strerror(errno));
		goto err_proc;
	}

	closedir(date_dir);
	date_dir = NULL;

	parray_qsort(backups, pgBackupCompareIdDesc);

	return backups;

err_proc:
	if (time_dir)
		closedir(time_dir);
	if (date_dir)
		closedir(date_dir);
	if (backup)
		pgBackupFree(backup);
	if (backups)
		parray_walk(backups, pgBackupFree);
	parray_free(backups);
	return NULL;
}

/*
 * Find the last completed database backup from the backup list.
 */
pgBackup *
catalog_get_last_data_backup(parray *backup_list, TimeLineID tli)
{
	int			i;
	pgBackup   *backup = NULL;

	/* backup_list is sorted in order of descending ID */
	for (i = 0; i < parray_num(backup_list); i++)
	{
		backup = (pgBackup *) parray_get(backup_list, i);

		/*
		 * We need completed database backup in the case of a full or
		 * differential backup on current timeline.
		 */
		if (backup->status == BACKUP_STATUS_OK &&
			backup->tli == tli &&
			(backup->backup_mode == BACKUP_MODE_DIFF_PAGE ||
			 backup->backup_mode == BACKUP_MODE_DIFF_PTRACK ||
			 backup->backup_mode == BACKUP_MODE_FULL))
			return backup;
	}

	return NULL;
}

/* create backup directory in $BACKUP_PATH */
int
pgBackupCreateDir(pgBackup *backup)
{
	int		i;
	char	path[MAXPGPATH];
	char   *subdirs[] = { DATABASE_DIR, NULL };

	pgBackupGetPath(backup, path, lengthof(path), NULL);
	dir_create_dir(path, DIR_PERMISSION);

	/* create directories for actual backup files */
	for (i = 0; subdirs[i]; i++)
	{
		pgBackupGetPath(backup, path, lengthof(path), subdirs[i]);
		dir_create_dir(path, DIR_PERMISSION);
	}

	return 0;
}

/*
 * Write configuration section of backup.in to stream "out".
 */
void
pgBackupWriteConfigSection(FILE *out, pgBackup *backup)
{
	static const char *modes[] = { "", "PAGE", "PTRACK", "FULL"};

	fprintf(out, "# configuration\n");
	fprintf(out, "BACKUP_MODE=%s\n", modes[backup->backup_mode]);
}

/*
 * Write result section of backup.in to stream "out".
 */
void
pgBackupWriteResultSection(FILE *out, pgBackup *backup)
{
	char timestamp[20];

	fprintf(out, "# result\n");
	fprintf(out, "TIMELINEID=%d\n", backup->tli);
	fprintf(out, "START_LSN=%x/%08x\n",
			(uint32) (backup->start_lsn >> 32),
			(uint32) backup->start_lsn);
	fprintf(out, "STOP_LSN=%x/%08x\n",
			(uint32) (backup->stop_lsn >> 32),
			(uint32) backup->stop_lsn);

	time2iso(timestamp, lengthof(timestamp), backup->start_time);
	fprintf(out, "START_TIME='%s'\n", timestamp);
	if (backup->end_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->end_time);
		fprintf(out, "END_TIME='%s'\n", timestamp);
	}
	fprintf(out, "RECOVERY_XID=%u\n", backup->recovery_xid);
	if (backup->recovery_time > 0)
	{
		time2iso(timestamp, lengthof(timestamp), backup->recovery_time);
		fprintf(out, "RECOVERY_TIME='%s'\n", timestamp);
	}

	if (backup->data_bytes != BYTES_INVALID)
		fprintf(out, "DATA_BYTES=" INT64_FORMAT "\n",
				backup->data_bytes);
	fprintf(out, "BLOCK_SIZE=%u\n", backup->block_size);
	fprintf(out, "XLOG_BLOCK_SIZE=%u\n", backup->wal_block_size);
	fprintf(out, "CHECKSUM_VERSION=%u\n", backup->checksum_version);
	fprintf(out, "STREAM=%u\n", backup->stream);

	fprintf(out, "STATUS=%s\n", status2str(backup->status));
	if (backup->parent_backup != 0)
	{
		char *parent_backup = base36enc(backup->parent_backup);
		fprintf(out, "PARENT_BACKUP='%s'\n", parent_backup);
		free(parent_backup);
	}
}

/* create backup.ini */
void
pgBackupWriteIni(pgBackup *backup)
{
	FILE   *fp = NULL;
	char	ini_path[MAXPGPATH];

	pgBackupGetPath(backup, ini_path, lengthof(ini_path), BACKUP_INI_FILE);
	fp = fopen(ini_path, "wt");
	if (fp == NULL)
		elog(ERROR, "cannot open INI file \"%s\": %s", ini_path,
			strerror(errno));

	/* configuration section */
	pgBackupWriteConfigSection(fp, backup);

	/* result section */
	pgBackupWriteResultSection(fp, backup);

	fclose(fp);
}

/*
 * Read backup.ini and create pgBackup.
 *  - Comment starts with ';'.
 *  - Do not care section.
 */
static pgBackup *
catalog_read_ini(const char *path)
{
	pgBackup   *backup;
	char	   *backup_mode = NULL;
	char	   *start_lsn = NULL;
	char	   *stop_lsn = NULL;
	char	   *status = NULL;
	char	   *parent_backup = NULL;
	int			i;

	pgut_option options[] =
	{
		{'s', 0, "backup-mode",			NULL, SOURCE_ENV},
		{'u', 0, "timelineid",			NULL, SOURCE_ENV},
		{'s', 0, "start-lsn",			NULL, SOURCE_ENV},
		{'s', 0, "stop-lsn",			NULL, SOURCE_ENV},
		{'t', 0, "start-time",			NULL, SOURCE_ENV},
		{'t', 0, "end-time",			NULL, SOURCE_ENV},
		{'U', 0, "recovery-xid",		NULL, SOURCE_ENV},
		{'t', 0, "recovery-time",		NULL, SOURCE_ENV},
		{'I', 0, "data-bytes",			NULL, SOURCE_ENV},
		{'u', 0, "block-size",			NULL, SOURCE_ENV},
		{'u', 0, "xlog-block-size",		NULL, SOURCE_ENV},
		{'u', 0, "checksum_version",	NULL, SOURCE_ENV},
		{'u', 0, "stream",				NULL, SOURCE_ENV},
		{'s', 0, "status",				NULL, SOURCE_ENV},
		{'s', 0, "parent_backup",		NULL, SOURCE_ENV},
		{0}
	};

	if (access(path, F_OK) != 0)
		return NULL;

	backup = pgut_new(pgBackup);
	catalog_init_config(backup);

	i = 0;
	options[i++].var = &backup_mode;
	options[i++].var = &backup->tli;
	options[i++].var = &start_lsn;
	options[i++].var = &stop_lsn;
	options[i++].var = &backup->start_time;
	options[i++].var = &backup->end_time;
	options[i++].var = &backup->recovery_xid;
	options[i++].var = &backup->recovery_time;
	options[i++].var = &backup->data_bytes;
	options[i++].var = &backup->block_size;
	options[i++].var = &backup->wal_block_size;
	options[i++].var = &backup->checksum_version;
	options[i++].var = &backup->stream;
	options[i++].var = &status;
	options[i++].var = &parent_backup;
	Assert(i == lengthof(options) - 1);

	pgut_readopt(path, options, ERROR);

	if (backup_mode)
	{
		backup->backup_mode = parse_backup_mode(backup_mode);
		free(backup_mode);
	}

	if (start_lsn)
	{
		uint32 xlogid;
		uint32 xrecoff;

		if (sscanf(start_lsn, "%X/%X", &xlogid, &xrecoff) == 2)
			backup->start_lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
		else
			elog(WARNING, "invalid START_LSN \"%s\"", start_lsn);
		free(start_lsn);
	}

	if (stop_lsn)
	{
		uint32 xlogid;
		uint32 xrecoff;

		if (sscanf(stop_lsn, "%X/%X", &xlogid, &xrecoff) == 2)
			backup->stop_lsn = (XLogRecPtr) ((uint64) xlogid << 32) | xrecoff;
		else
			elog(WARNING, "invalid STOP_LSN \"%s\"", stop_lsn);
		free(stop_lsn);
	}

	if (status)
	{
		if (strcmp(status, "OK") == 0)
			backup->status = BACKUP_STATUS_OK;
		else if (strcmp(status, "RUNNING") == 0)
			backup->status = BACKUP_STATUS_RUNNING;
		else if (strcmp(status, "ERROR") == 0)
			backup->status = BACKUP_STATUS_ERROR;
		else if (strcmp(status, "DELETING") == 0)
			backup->status = BACKUP_STATUS_DELETING;
		else if (strcmp(status, "DELETED") == 0)
			backup->status = BACKUP_STATUS_DELETED;
		else if (strcmp(status, "DONE") == 0)
			backup->status = BACKUP_STATUS_DONE;
		else if (strcmp(status, "CORRUPT") == 0)
			backup->status = BACKUP_STATUS_CORRUPT;
		else
			elog(WARNING, "invalid STATUS \"%s\"", status);
		free(status);
	}

	if (parent_backup)
	{
		backup->parent_backup = base36dec(parent_backup);
		free(parent_backup);
	}

	return backup;
}

BackupMode
parse_backup_mode(const char *value)
{
	const char *v = value;
	size_t		len;

	/* Skip all spaces detected */
	while (IsSpace(*v))
		v++;
	len = strlen(v);

	if (len > 0 && pg_strncasecmp("full", v, strlen("full")) == 0)
		return BACKUP_MODE_FULL;
	else if (len > 0 && pg_strncasecmp("page", v, strlen("page")) == 0)
		return BACKUP_MODE_DIFF_PAGE;
	else if (len > 0 && pg_strncasecmp("ptrack", v, strlen("ptrack")) == 0)
		return BACKUP_MODE_DIFF_PTRACK;

	/* Backup mode is invalid, so leave with an error */
	elog(ERROR, "invalid backup-mode \"%s\"", value);
	return BACKUP_MODE_INVALID;
}

/* free pgBackup object */
void
pgBackupFree(void *backup)
{
	free(backup);
}

/* Compare two pgBackup with their IDs (start time) in ascending order */
int
pgBackupCompareId(const void *l, const void *r)
{
	pgBackup *lp = *(pgBackup **)l;
	pgBackup *rp = *(pgBackup **)r;

	if (lp->start_time > rp->start_time)
		return 1;
	else if (lp->start_time < rp->start_time)
		return -1;
	else
		return 0;
}

/* Compare two pgBackup with their IDs in descending order */
int
pgBackupCompareIdDesc(const void *l, const void *r)
{
	return -pgBackupCompareId(l, r);
}

/*
 * Construct absolute path of the backup directory.
 * If subdir is not NULL, it will be appended after the path.
 */
void
pgBackupGetPath(const pgBackup *backup, char *path, size_t len, const char *subdir)
{
	char	*datetime;

	datetime = base36enc(backup->start_time);
	if (subdir)
		snprintf(path, len, "%s/%s/%s/%s", backup_path, BACKUPS_DIR, datetime, subdir);
	else
		snprintf(path, len, "%s/%s/%s", backup_path, BACKUPS_DIR, datetime);
	free(datetime);
}

void
catalog_init_config(pgBackup *backup)
{
	backup->backup_mode = BACKUP_MODE_INVALID;
	backup->status = BACKUP_STATUS_INVALID;
	backup->tli = 0;
	backup->start_lsn = 0;
	backup->stop_lsn = 0;
	backup->start_time = (time_t) 0;
	backup->end_time = (time_t) 0;
	backup->recovery_xid = 0;
	backup->recovery_time = (time_t) 0;
	backup->data_bytes = BYTES_INVALID;
	backup->stream = false;
	backup->parent_backup = 0;
}
