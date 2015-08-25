/*****************************************************************************

Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Google Inc.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file log/log0log.cc
Database log

Created 12/9/1995 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"

#include "log0log.h"

#ifdef UNIV_NONINL
#include "log0log.ic"
#endif

#ifndef UNIV_HOTBACKUP
#include "mem0mem.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "srv0srv.h"
#include "log0archive.h"
#include "log0recv.h"
#include "fil0fil.h"
#include "dict0boot.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "srv0mon.h"
#include "sync0sync.h"

/*
General philosophy of InnoDB redo-logs:

1) Every change to a contents of a data page must be done
through mtr, which in mtr_commit() writes log records
to the InnoDB redo log.

2) Normally these changes are performed using a mlog_write_ulint()
or similar function.

3) In some page level operations only a code number of a
c-function and its parameters are written to the log to
reduce the size of the log.

  3a) You should not add parameters to these kind of functions
  (e.g. trx_undo_header_create(), trx_undo_insert_header_reuse())

  3b) You should not add such functionality which either change
  working when compared with the old or are dependent on data
  outside of the page. These kind of functions should implement
  self-contained page transformation and it should be unchanged
  if you don't have very essential reasons to change log
  semantics or format.

*/

/* Global log system variable */
log_t*	log_sys	= NULL;

/** Pointer to the log checksum calculation function */
log_checksum_func_t log_checksum_algorithm_ptr =
	log_block_calc_checksum_innodb;

/* These control how often we print warnings if the last checkpoint is too
old */
ibool	log_has_printed_chkp_warning = FALSE;
time_t	log_last_warning_time;

/* A margin for free space in the log buffer before a log entry is catenated */
#define LOG_BUF_WRITE_MARGIN	(4 * OS_FILE_LOG_BLOCK_SIZE)

/* Margins for free space in the log buffer after a log entry is catenated */
#define LOG_BUF_FLUSH_RATIO	2
#define LOG_BUF_FLUSH_MARGIN	(LOG_BUF_WRITE_MARGIN + 4 * UNIV_PAGE_SIZE)

/* Margin for the free space in the smallest log group, before a new query
step which modifies the database, is started */

#define LOG_CHECKPOINT_FREE_PER_THREAD	(4 * UNIV_PAGE_SIZE)
#define LOG_CHECKPOINT_EXTRA_FREE	(8 * UNIV_PAGE_SIZE)

/* This parameter controls asynchronous making of a new checkpoint; the value
should be bigger than LOG_POOL_PREFLUSH_RATIO_SYNC */

#define LOG_POOL_CHECKPOINT_RATIO_ASYNC	32

/* This parameter controls synchronous preflushing of modified buffer pages */
#define LOG_POOL_PREFLUSH_RATIO_SYNC	16

/* The same ratio for asynchronous preflushing; this value should be less than
the previous */
#define LOG_POOL_PREFLUSH_RATIO_ASYNC	8

/* Codes used in unlocking flush latches */
#define LOG_UNLOCK_NONE_FLUSHED_LOCK	1
#define LOG_UNLOCK_FLUSH_LOCK		2

/******************************************************//**
Completes a checkpoint write i/o to a log file. */
static
void
log_io_complete_checkpoint(void);
/*============================*/

/****************************************************************//**
Returns the oldest modified block lsn in the pool, or log_sys->lsn if none
exists.
@return LSN of oldest modification */
static
lsn_t
log_buf_pool_get_oldest_modification(void)
/*======================================*/
{
	lsn_t	lsn;

	ut_ad(log_mutex_own());

	lsn = buf_pool_get_oldest_modification();

	if (!lsn) {

		lsn = log_sys->lsn;
	}

	return(lsn);
}

/****************************************************************//**
Checks if the log groups have a big enough margin of free space in
so that a new log entry can be written without overwriting log data
that is not read by the changed page bitmap thread.
@return TRUE if there is not enough free space. */
static
ibool
log_check_tracking_margin(
	ulint	lsn_advance)	/*!< in: an upper limit on how much log data we
				plan to write.  If zero, the margin will be
				checked for the already-written log. */
{
	lsn_t	tracked_lsn;
	lsn_t	tracked_lsn_age;

	if (!srv_track_changed_pages) {
		return FALSE;
	}

	ut_ad(mutex_own(&(log_sys->mutex)));

	tracked_lsn = log_get_tracked_lsn();
	tracked_lsn_age = log_sys->lsn - tracked_lsn;

	/* The overwrite would happen when log_sys->log_group_capacity is
	exceeded, but we use max_checkpoint_age for an extra safety margin. */
	return tracked_lsn_age + lsn_advance > log_sys->max_checkpoint_age;
}

/** Extends the log buffer.
@param[in]	len	requested minimum size in bytes */

void
log_buffer_extend(
	ulint	len)
{
	ulint	move_start;
	ulint	move_end;
	byte*	tmp_buf[OS_FILE_LOG_BLOCK_SIZE];

	log_mutex_enter();

	while (log_sys->is_extending) {
		/* Another thread is trying to extend already.
		Needs to wait for. */
		log_mutex_exit();

		log_buffer_flush_to_disk();

		log_mutex_enter();

		if (srv_log_buffer_size > len / UNIV_PAGE_SIZE) {
			/* Already extended enough by the others */
			log_mutex_exit();
			return;
		}
	}

	if (len >= log_sys->buf_size / 2) {
		DBUG_EXECUTE_IF("ib_log_buffer_is_short_crash",
				DBUG_SUICIDE(););

		/* log_buffer is too small. try to extend instead of crash. */
		ib_logf(IB_LOG_LEVEL_WARN,
			"The transaction log size is too large"
			" for innodb_log_buffer_size (%lu >= %lu / 2)."
			" Trying to extend it.",
			len, LOG_BUFFER_SIZE);
	}

	log_sys->is_extending = true;

	while (ut_calc_align_down(log_sys->buf_free,
				  OS_FILE_LOG_BLOCK_SIZE)
	       != ut_calc_align_down(log_sys->buf_next_to_write,
				     OS_FILE_LOG_BLOCK_SIZE)) {
		/* Buffer might have >1 blocks to write still. */
		log_mutex_exit();

		log_buffer_flush_to_disk();

		log_mutex_enter();
	}

	move_start = ut_calc_align_down(
		log_sys->buf_free,
		OS_FILE_LOG_BLOCK_SIZE);
	move_end = log_sys->buf_free;

	/* store the last log block in buffer */
	ut_memcpy(tmp_buf, log_sys->buf + move_start,
		  move_end - move_start);

	log_sys->buf_free -= move_start;
	log_sys->buf_next_to_write -= move_start;

	/* reallocate log buffer */
	srv_log_buffer_size = len / UNIV_PAGE_SIZE + 1;
	ut_free(log_sys->buf_ptr);
	log_sys->buf_ptr = static_cast<byte*>(
		ut_zalloc_nokey(LOG_BUFFER_SIZE + OS_FILE_LOG_BLOCK_SIZE));
	log_sys->buf = static_cast<byte*>(
		ut_align(log_sys->buf_ptr, OS_FILE_LOG_BLOCK_SIZE));
	log_sys->buf_size = LOG_BUFFER_SIZE;
	log_sys->max_buf_free = log_sys->buf_size / LOG_BUF_FLUSH_RATIO
		- LOG_BUF_FLUSH_MARGIN;

	/* restore the last log block */
	ut_memcpy(log_sys->buf, tmp_buf, move_end - move_start);

	ut_ad(log_sys->is_extending);
	log_sys->is_extending = false;

	log_mutex_exit();

	ib_logf(IB_LOG_LEVEL_INFO,
		"innodb_log_buffer_size was extended to %lu.",
		LOG_BUFFER_SIZE);
}

/** Open the log for log_write_low. The log must be closed with log_close.
@param[in]	len	length of the data to be written
@return start lsn of the log record */

lsn_t
log_reserve_and_open(
	ulint	len)
{
	ulint	len_upper_limit;
#ifdef UNIV_LOG_ARCHIVE
	ulint	archived_lsn_age;
	ulint	dummy;
#endif /* UNIV_LOG_ARCHIVE */
	ulint	count			= 0;
	ulint	tcount			= 0;

loop:
	ut_ad(log_mutex_own());
	ut_ad(!recv_no_log_write);

	if (log_sys->is_extending) {
		log_mutex_exit();

		/* Log buffer size is extending. Writing up to the next block
		should wait for the extending finished. */

		os_thread_sleep(100000);

		ut_ad(++count < 50);

		log_mutex_enter();
		goto loop;
	}

	/* Calculate an upper limit for the space the string may take in the
	log buffer */

	len_upper_limit = LOG_BUF_WRITE_MARGIN + srv_log_write_ahead_size
			  + (5 * len) / 4;

	if (log_sys->buf_free + len_upper_limit > log_sys->buf_size) {
		log_mutex_exit();

		/* Not enough free space, do a write of the log buffer */

		log_buffer_sync_in_background(false);

		srv_stats.log_waits.inc();

		ut_ad(++count < 50);

		log_mutex_enter();
		goto loop;
	}

	if (log_sys->archiving_state != LOG_ARCH_OFF) {

		archived_lsn_age = log_sys->lsn - log_sys->archived_lsn;
		if (archived_lsn_age + len_upper_limit
		    > log_sys->max_archived_lsn_age) {
			/* Not enough free archived space in log groups: do a
			synchronous archive write batch: */

			log_mutex_exit();

			ut_ad(len_upper_limit
			      <= log_sys->max_archived_lsn_age);

			log_archive_do(TRUE, &dummy);

			ut_ad(++count < 50);

			goto loop;
		}
	}

	if (log_check_tracking_margin(len_upper_limit) &&
		(++tcount + count < 50)) {

		/* This log write would violate the untracked LSN free space
		margin.  Limit this to 50 retries as there might be situations
		where we have no choice but to proceed anyway, i.e. if the log
		is about to be overflown, log tracking or not. */
		log_mutex_exit();

		os_thread_sleep(10000);

		goto loop;
	}

	return(log_sys->lsn);
}

/************************************************************//**
Writes to the log the string given. It is assumed that the caller holds the
log mutex. */

void
log_write_low(
/*==========*/
	const byte*	str,		/*!< in: string */
	ulint		str_len)	/*!< in: string length */
{
	log_t*	log	= log_sys;
	ulint	len;
	ulint	data_len;
	byte*	log_block;

	ut_ad(log_mutex_own());
part_loop:
	ut_ad(!recv_no_log_write);
	/* Calculate a part length */

	data_len = (log->buf_free % OS_FILE_LOG_BLOCK_SIZE) + str_len;

	if (data_len <= OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {

		/* The string fits within the current log block */

		len = str_len;
	} else {
		data_len = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE;

		len = OS_FILE_LOG_BLOCK_SIZE
			- (log->buf_free % OS_FILE_LOG_BLOCK_SIZE)
			- LOG_BLOCK_TRL_SIZE;
	}

	ut_memcpy(log->buf + log->buf_free, str, len);

	str_len -= len;
	str = str + len;

	log_block = static_cast<byte*>(
		ut_align_down(
			log->buf + log->buf_free, OS_FILE_LOG_BLOCK_SIZE));

	log_block_set_data_len(log_block, data_len);

	if (data_len == OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {
		/* This block became full */
		log_block_set_data_len(log_block, OS_FILE_LOG_BLOCK_SIZE);
		log_block_set_checkpoint_no(log_block,
					    log_sys->next_checkpoint_no);
		len += LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE;

		log->lsn += len;

		/* Initialize the next block header */
		log_block_init(log_block + OS_FILE_LOG_BLOCK_SIZE, log->lsn);
	} else {
		log->lsn += len;
	}

	log->buf_free += len;

	ut_ad(log->buf_free <= log->buf_size);

	if (str_len > 0) {
		goto part_loop;
	}

	srv_stats.log_write_requests.inc();
}

/************************************************************//**
Closes the log.
@return lsn */

lsn_t
log_close(void)
/*===========*/
{
	byte*		log_block;
	ulint		first_rec_group;
	lsn_t		oldest_lsn;
	lsn_t		lsn;
	lsn_t		tracked_lsn;
	lsn_t		tracked_lsn_age;
	log_t*		log	= log_sys;
	lsn_t		checkpoint_age;

	ut_ad(log_mutex_own());
	ut_ad(!recv_no_log_write);

	lsn = log->lsn;

	log_block = static_cast<byte*>(
		ut_align_down(
			log->buf + log->buf_free, OS_FILE_LOG_BLOCK_SIZE));

	first_rec_group = log_block_get_first_rec_group(log_block);

	if (first_rec_group == 0) {
		/* We initialized a new log block which was not written
		full by the current mtr: the next mtr log record group
		will start within this block at the offset data_len */

		log_block_set_first_rec_group(
			log_block, log_block_get_data_len(log_block));
	}

	if (log->buf_free > log->max_buf_free) {

		log->check_flush_or_checkpoint = true;
	}

	if (srv_track_changed_pages) {

		tracked_lsn = log_get_tracked_lsn();
		tracked_lsn_age = lsn - tracked_lsn;

		if (tracked_lsn_age >= log->log_group_capacity) {

			fprintf(stderr, "InnoDB: Error: the age of the "
				"oldest untracked record exceeds the log "
				"group capacity!\n");
			fprintf(stderr, "InnoDB: Error: stopping the log "
				"tracking thread at LSN " LSN_PF "\n",
				tracked_lsn);
			srv_track_changed_pages = FALSE;
		}
	}

	checkpoint_age = lsn - log->last_checkpoint_lsn;

	if (checkpoint_age >= log->log_group_capacity) {

		if (!log_has_printed_chkp_warning
		    || difftime(time(NULL), log_last_warning_time) > 15) {

			log_has_printed_chkp_warning = TRUE;
			log_last_warning_time = time(NULL);

			ib_logf(IB_LOG_LEVEL_ERROR,
				"The age of the last checkpoint is"
				" " LSN_PF ", which exceeds the log group"
				" capacity " LSN_PF ".", checkpoint_age,
				log->log_group_capacity);
		}
	}

	if (checkpoint_age <= log->max_modified_age_sync) {

		goto function_exit;
	}

	oldest_lsn = buf_pool_get_oldest_modification();

	if (!oldest_lsn
	    || lsn - oldest_lsn > log->max_modified_age_sync
	    || checkpoint_age > log->max_checkpoint_age_async) {

		log->check_flush_or_checkpoint = true;
	}
function_exit:

	return(lsn);
}

/******************************************************//**
Calculates the data capacity of a log group, when the log file headers are not
included.
@return capacity in bytes */

lsn_t
log_group_get_capacity(
/*===================*/
	const log_group_t*	group)	/*!< in: log group */
{
	ut_ad(log_mutex_own());

	return((group->file_size - LOG_FILE_HDR_SIZE) * group->n_files);
}

/******************************************************//**
Calculates the offset within a log group, when the log file headers are not
included.
@return size offset (<= offset) */
UNIV_INLINE
lsn_t
log_group_calc_size_offset(
/*=======================*/
	lsn_t			offset,	/*!< in: real offset within the
					log group */
	const log_group_t*	group)	/*!< in: log group */
{
	ut_ad(log_mutex_own());

	return(offset - LOG_FILE_HDR_SIZE * (1 + offset / group->file_size));
}

/******************************************************//**
Calculates the offset within a log group, when the log file headers are
included.
@return real offset (>= offset) */
UNIV_INLINE
lsn_t
log_group_calc_real_offset(
/*=======================*/
	lsn_t			offset,	/*!< in: size offset within the
					log group */
	const log_group_t*	group)	/*!< in: log group */
{
	ut_ad(log_mutex_own());

	return(offset + LOG_FILE_HDR_SIZE
	       * (1 + offset / (group->file_size - LOG_FILE_HDR_SIZE)));
}

/******************************************************//**
Calculates the offset of an lsn within a log group.
@return offset within the log group */
static
lsn_t
log_group_calc_lsn_offset(
/*======================*/
	lsn_t			lsn,	/*!< in: lsn */
	const log_group_t*	group)	/*!< in: log group */
{
	lsn_t	gr_lsn;
	lsn_t	gr_lsn_size_offset;
	lsn_t	difference;
	lsn_t	group_size;
	lsn_t	offset;

	ut_ad(log_mutex_own());

	gr_lsn = group->lsn;

	gr_lsn_size_offset = log_group_calc_size_offset(group->lsn_offset, group);

	group_size = log_group_get_capacity(group);

	if (lsn >= gr_lsn) {

		difference = lsn - gr_lsn;
	} else {
		difference = gr_lsn - lsn;

		difference = difference % group_size;

		difference = group_size - difference;
	}

	offset = (gr_lsn_size_offset + difference) % group_size;

	/* fprintf(stderr,
	"Offset is " LSN_PF " gr_lsn_offset is " LSN_PF
	" difference is " LSN_PF "\n",
	offset, gr_lsn_size_offset, difference);
	*/

	return(log_group_calc_real_offset(offset, group));
}
#endif /* !UNIV_HOTBACKUP */

/*******************************************************************//**
Calculates where in log files we find a specified lsn.
@return log file number */

ulint
log_calc_where_lsn_is(
/*==================*/
	int64_t*	log_file_offset,	/*!< out: offset in that file
						(including the header) */
	ib_uint64_t	first_header_lsn,	/*!< in: first log file start
						lsn */
	ib_uint64_t	lsn,			/*!< in: lsn whose position to
						determine */
	ulint		n_log_files,		/*!< in: total number of log
						files */
	int64_t		log_file_size)		/*!< in: log file size
						(including the header) */
{
	int64_t		capacity	= log_file_size - LOG_FILE_HDR_SIZE;
	ulint		file_no;
	int64_t		add_this_many;

	if (lsn < first_header_lsn) {
		add_this_many = 1 + (first_header_lsn - lsn)
			/ (capacity * static_cast<int64_t>(n_log_files));
		lsn += add_this_many
			* capacity * static_cast<int64_t>(n_log_files);
	}

	ut_a(lsn >= first_header_lsn);

	file_no = ((ulint)((lsn - first_header_lsn) / capacity))
		% n_log_files;
	*log_file_offset = (lsn - first_header_lsn) % capacity;

	*log_file_offset = *log_file_offset + LOG_FILE_HDR_SIZE;

	return(file_no);
}

#ifndef UNIV_HOTBACKUP
/********************************************************//**
Sets the field values in group to correspond to a given lsn. For this function
to work, the values must already be correctly initialized to correspond to
some lsn, for instance, a checkpoint lsn. */

void
log_group_set_fields(
/*=================*/
	log_group_t*	group,	/*!< in/out: group */
	lsn_t		lsn)	/*!< in: lsn for which the values should be
				set */
{
	group->lsn_offset = log_group_calc_lsn_offset(lsn, group);
	group->lsn = lsn;
}

/* Extra margin, in addition to one log file, used in archiving */
#define LOG_ARCHIVE_EXTRA_MARGIN	(4 * UNIV_PAGE_SIZE)

/* This parameter controls asynchronous writing to the archive */
#define LOG_ARCHIVE_RATIO_ASYNC		16

/*****************************************************************//**
Calculates the recommended highest values for lsn - last_checkpoint_lsn
and lsn - buf_get_oldest_modification().
@retval true on success
@retval false if the smallest log group is too small to
accommodate the number of OS threads in the database server */
static __attribute__((warn_unused_result))
bool
log_calc_max_ages(void)
/*===================*/
{
	log_group_t*	group;
	lsn_t		margin;
	ulint		free;
	bool		success	= true;
	lsn_t		smallest_capacity;
	lsn_t		archive_margin;
	lsn_t		smallest_archive_margin;

	log_mutex_enter();

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	ut_ad(group);

	smallest_capacity = LSN_MAX;
	smallest_archive_margin = LSN_MAX;

	while (group) {
		if (log_group_get_capacity(group) < smallest_capacity) {

			smallest_capacity = log_group_get_capacity(group);
		}

		archive_margin = log_group_get_capacity(group)
		    - (group->file_size - LOG_FILE_HDR_SIZE)
		    - LOG_ARCHIVE_EXTRA_MARGIN;

		if (archive_margin < smallest_archive_margin) {

		    smallest_archive_margin = archive_margin;
		}

		group = UT_LIST_GET_NEXT(log_groups, group);
	}

	/* Add extra safety */
	smallest_capacity = smallest_capacity - smallest_capacity / 10;

	/* For each OS thread we must reserve so much free space in the
	smallest log group that it can accommodate the log entries produced
	by single query steps: running out of free log space is a serious
	system error which requires rebooting the database. */

	free = LOG_CHECKPOINT_FREE_PER_THREAD * (10 + srv_thread_concurrency)
		+ LOG_CHECKPOINT_EXTRA_FREE;
	if (free >= smallest_capacity / 2) {
		success = false;

		goto failure;
	} else {
		margin = smallest_capacity - free;
	}

	margin = margin - margin / 10;	/* Add still some extra safety */

	log_sys->log_group_capacity = smallest_capacity;

	log_sys->max_modified_age_async = margin
		- margin / LOG_POOL_PREFLUSH_RATIO_ASYNC;
	log_sys->max_modified_age_sync = margin
		- margin / LOG_POOL_PREFLUSH_RATIO_SYNC;

	log_sys->max_checkpoint_age_async = margin - margin
		/ LOG_POOL_CHECKPOINT_RATIO_ASYNC;
	log_sys->max_checkpoint_age = margin;

	log_sys->max_archived_lsn_age = smallest_archive_margin;

	log_sys->max_archived_lsn_age_async = smallest_archive_margin
	    - smallest_archive_margin / LOG_ARCHIVE_RATIO_ASYNC;
failure:
	log_mutex_exit();

	if (!success) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot continue operation. ib_logfiles are too"
			" small for innodb_thread_concurrency %lu. The"
			" combined size of ib_logfiles should be bigger than"
			" 200 kB * innodb_thread_concurrency. To get mysqld"
			" to start up, set innodb_thread_concurrency in"
			" my.cnf to a lower value, for example, to 8. After"
			" an ERROR-FREE shutdown of mysqld you can adjust"
			" the size of ib_logfiles. %s",
			(ulong) srv_thread_concurrency, INNODB_PARAMETERS_MSG);
	}

	return(success);
}

/******************************************************//**
Initializes the log. */

void
log_init(void)
/*==========*/
{
	log_sys = static_cast<log_t*>(ut_zalloc_nokey(sizeof(log_t)));

	mutex_create("log_sys", &log_sys->mutex);

	mutex_create("log_flush_order", &log_sys->log_flush_order_mutex);

	/* Start the lsn from one log block from zero: this way every
	log record has a start lsn != zero, a fact which we will use */

	log_sys->lsn = LOG_START_LSN;

	ut_a(LOG_BUFFER_SIZE >= 16 * OS_FILE_LOG_BLOCK_SIZE);
	ut_a(LOG_BUFFER_SIZE >= 4 * UNIV_PAGE_SIZE);

	log_sys->buf_ptr = static_cast<byte*>(
		ut_zalloc_nokey(LOG_BUFFER_SIZE + OS_FILE_LOG_BLOCK_SIZE));

	log_sys->buf = static_cast<byte*>(
		ut_align(log_sys->buf_ptr, OS_FILE_LOG_BLOCK_SIZE));

	log_sys->buf_size = LOG_BUFFER_SIZE;

	log_sys->max_buf_free = log_sys->buf_size / LOG_BUF_FLUSH_RATIO
		- LOG_BUF_FLUSH_MARGIN;
	log_sys->check_flush_or_checkpoint = true;
	UT_LIST_INIT(log_sys->log_groups, &log_group_t::log_groups);

	log_sys->n_log_ios_old = log_sys->n_log_ios;
	log_sys->last_printout_time = time(NULL);
	/*----------------------------*/

	log_sys->write_lsn = log_sys->lsn;

	log_sys->flush_event = os_event_create(0);

	os_event_set(log_sys->flush_event);

	/*----------------------------*/

	log_sys->last_checkpoint_lsn = log_sys->lsn;

	rw_lock_create(
		checkpoint_lock_key, &log_sys->checkpoint_lock,
		SYNC_NO_ORDER_CHECK);

	log_sys->checkpoint_buf_ptr = static_cast<byte*>(
		ut_zalloc_nokey(2 * OS_FILE_LOG_BLOCK_SIZE));

	log_sys->checkpoint_buf = static_cast<byte*>(
		ut_align(log_sys->checkpoint_buf_ptr, OS_FILE_LOG_BLOCK_SIZE));

	/*----------------------------*/

	/* Under MySQL, log archiving is always off */
	log_sys->archiving_state = LOG_ARCH_OFF;
	log_sys->archived_lsn = log_sys->lsn;
	log_sys->next_archived_lsn = 0;

	log_sys->n_pending_archive_ios = 0;

	rw_lock_create(archive_lock_key, &log_sys->archive_lock,
		       SYNC_NO_ORDER_CHECK);

	log_sys->archive_buf_ptr = static_cast<byte*>(
		ut_zalloc_nokey(LOG_ARCHIVE_BUF_SIZE
				+ OS_FILE_LOG_BLOCK_SIZE));

	log_sys->archive_buf = static_cast<byte*>(
		ut_align(log_sys->archive_buf_ptr, OS_FILE_LOG_BLOCK_SIZE));

	log_sys->archive_buf_size = LOG_ARCHIVE_BUF_SIZE;

	log_sys->archiving_on = os_event_create(0); // TODO laurynas annotate?

	log_sys->tracked_lsn = 0;

	/*----------------------------*/

	log_block_init(log_sys->buf, log_sys->lsn);
	log_block_set_first_rec_group(log_sys->buf, LOG_BLOCK_HDR_SIZE);

	log_sys->buf_free = LOG_BLOCK_HDR_SIZE;
	log_sys->lsn = LOG_START_LSN + LOG_BLOCK_HDR_SIZE;

	MONITOR_SET(MONITOR_LSN_CHECKPOINT_AGE,
		    log_sys->lsn - log_sys->last_checkpoint_lsn);
}

/******************************************************************//**
Inits a log group to the log system.
@return true if success, false if not */
__attribute__((warn_unused_result))
bool
log_group_init(
/*===========*/
	ulint	id,			/*!< in: group id */
	ulint	n_files,		/*!< in: number of log files */
	lsn_t	file_size,		/*!< in: log file size in bytes */
	ulint	space_id,		/*!< in: space id of the file space
					which contains the log files of this
					group */
	ulint	archive_space_id)
					/*!< in: space id of the file space
					which contains some archived log
					files for this group; currently, only
					for the first log group this is
					used */
{
	ulint	i;
	log_group_t*	group;

	group = static_cast<log_group_t*>(ut_malloc_nokey(sizeof(log_group_t)));

	group->id = id;
	group->n_files = n_files;
	group->file_size = file_size;
	group->space_id = space_id;
	group->state = LOG_GROUP_OK;
	group->lsn = LOG_START_LSN;
	group->lsn_offset = LOG_FILE_HDR_SIZE;

	group->file_header_bufs_ptr = static_cast<byte**>(
		ut_zalloc_nokey(sizeof(byte*) * n_files));

	group->file_header_bufs = static_cast<byte**>(
		ut_zalloc_nokey(sizeof(byte**) * n_files));

	group->archive_file_header_bufs_ptr = static_cast<byte**>(
		ut_zalloc_nokey(sizeof(byte*) * n_files));

	group->archive_file_header_bufs = static_cast<byte**>(
		ut_zalloc_nokey(sizeof(byte*) * n_files));

	for (i = 0; i < n_files; i++) {
		group->file_header_bufs_ptr[i] = static_cast<byte*>(
			ut_zalloc_nokey(LOG_FILE_HDR_SIZE
					+ OS_FILE_LOG_BLOCK_SIZE));

		group->file_header_bufs[i] = static_cast<byte*>(
			ut_align(group->file_header_bufs_ptr[i],
				 OS_FILE_LOG_BLOCK_SIZE));

		group->archive_file_header_bufs_ptr[i] = static_cast<byte*>(
			ut_zalloc_nokey(LOG_FILE_HDR_SIZE
					+ OS_FILE_LOG_BLOCK_SIZE));

		group->archive_file_header_bufs[i] = static_cast<byte*>(
			ut_align(group->archive_file_header_bufs_ptr[i],
				 OS_FILE_LOG_BLOCK_SIZE));

	}

	group->archive_space_id = archive_space_id;

	group->archived_file_no = LOG_START_LSN;
	group->archived_offset = 0;

	group->checkpoint_buf_ptr = static_cast<byte*>(
		ut_zalloc_nokey(2 * OS_FILE_LOG_BLOCK_SIZE));

	group->checkpoint_buf = static_cast<byte*>(
		ut_align(group->checkpoint_buf_ptr,OS_FILE_LOG_BLOCK_SIZE));

	UT_LIST_ADD_LAST(log_sys->log_groups, group);

	return(log_calc_max_ages());
}

/******************************************************//**
Update log_sys after write completion. */
static
void
log_sys_write_completion(void)
/*==========================*/
{
	ulint	move_start;
	ulint	move_end;

	ut_ad(log_mutex_own());

	log_sys->write_lsn = log_sys->lsn;
	log_sys->buf_next_to_write = log_sys->write_end_offset;

	if (log_sys->write_end_offset > log_sys->max_buf_free / 2) {
		/* Move the log buffer content to the start of the
		buffer */

		move_start = ut_calc_align_down(
			log_sys->write_end_offset,
			OS_FILE_LOG_BLOCK_SIZE);
		move_end = ut_calc_align(log_sys->buf_free,
					 OS_FILE_LOG_BLOCK_SIZE);

		ut_memmove(log_sys->buf, log_sys->buf + move_start,
			   move_end - move_start);
		log_sys->buf_free -= move_start;

		log_sys->buf_next_to_write -= move_start;
	}
}

/******************************************************//**
Completes an i/o to a log file. */

void
log_io_complete(
/*============*/
	log_group_t*	group)	/*!< in: log group or a dummy pointer */
{

	if ((byte*) group == &log_archive_io) {
		/* It was an archive write */

		log_io_complete_archive();

		return;
	}

	if ((ulint) group & 0x1UL) {
		/* It was a checkpoint write */
		group = (log_group_t*)((ulint) group - 1);

#ifdef _WIN32
		fil_flush(group->space_id);
#else
		switch (srv_unix_file_flush_method) {
		case SRV_UNIX_O_DSYNC:
		case SRV_UNIX_NOSYNC:
		case SRV_UNIX_ALL_O_DIRECT:
			break;
		case SRV_UNIX_FSYNC:
		case SRV_UNIX_LITTLESYNC:
		case SRV_UNIX_O_DIRECT:
		case SRV_UNIX_O_DIRECT_NO_FSYNC:
			if (thd_flush_log_at_trx_commit(NULL) != 2)
				fil_flush(group->space_id);
		}
#endif /* _WIN32 */

		DBUG_PRINT("ib_log", ("checkpoint info written to group %u",
				      unsigned(group->id)));
		log_io_complete_checkpoint();

		return;
	}

	ut_error;	/*!< We currently use synchronous writing of the
			logs and cannot end up here! */
}

/******************************************************//**
Writes a log file header to a log file space. */
static
void
log_group_file_header_flush(
/*========================*/
	log_group_t*	group,		/*!< in: log group */
	ulint		nth_file,	/*!< in: header to the nth file in the
					log file space */
	lsn_t		start_lsn)	/*!< in: log file data starts at this
					lsn */
{
	byte*	buf;
	lsn_t	dest_offset;

	ut_ad(log_mutex_own());
	ut_ad(!recv_no_log_write);
	ut_a(nth_file < group->n_files);

	buf = *(group->file_header_bufs + nth_file);

	mach_write_to_4(buf + LOG_GROUP_ID, group->id);
	mach_write_to_8(buf + LOG_FILE_START_LSN, start_lsn);

	/* Wipe over possible label of mysqlbackup --restore */
	memset(buf + LOG_FILE_WAS_CREATED_BY_HOT_BACKUP, 0x20, 4);

	dest_offset = nth_file * group->file_size;

	DBUG_PRINT("ib_log", ("write " LSN_PF
			      " group " ULINTPF
			      " file " ULINTPF " header",
			      start_lsn, group->id, nth_file));

	log_sys->n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	srv_stats.os_log_pending_writes.inc();

	const ulint	page_no
		= (ulint) (dest_offset / univ_page_size.physical());

	fil_io(OS_FILE_WRITE | OS_FILE_LOG, true,
	       page_id_t(group->space_id, page_no),
	       univ_page_size,
	       (ulint) (dest_offset % univ_page_size.physical()),
	       OS_FILE_LOG_BLOCK_SIZE, buf, group);

	srv_stats.os_log_pending_writes.dec();
}

/******************************************************//**
Stores a 4-byte checksum to the trailer checksum field of a log block
before writing it to a log file. This checksum is used in recovery to
check the consistency of a log block. */
static
void
log_block_store_checksum(
/*=====================*/
	byte*	block)	/*!< in/out: pointer to a log block */
{
	log_block_set_checksum(block, log_block_calc_checksum(block));
}

/******************************************************//**
Writes a buffer to a log file group. */

void
log_group_write_buf(
/*================*/
	log_group_t*	group,		/*!< in: log group */
	byte*		buf,		/*!< in: buffer */
	ulint		len,		/*!< in: buffer len; must be divisible
					by OS_FILE_LOG_BLOCK_SIZE */
#ifdef UNIV_DEBUG
	ulint		pad_len,	/*!< in: pad len in the buffer len */
#endif /* UNIV_DEBUG */
	lsn_t		start_lsn,	/*!< in: start lsn of the buffer; must
					be divisible by
					OS_FILE_LOG_BLOCK_SIZE */
	ulint		new_data_offset)/*!< in: start offset of new data in
					buf: this parameter is used to decide
					if we have to write a new log file
					header */
{
	ulint		write_len;
	bool		write_header	= new_data_offset == 0;
	lsn_t		next_offset;
	ulint		i;

	ut_ad(log_mutex_own());
	ut_ad(!recv_no_log_write);
	ut_a(len % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_a(start_lsn % OS_FILE_LOG_BLOCK_SIZE == 0);

loop:
	if (len == 0) {

		return;
	}

	next_offset = log_group_calc_lsn_offset(start_lsn, group);

	if (write_header
	    && next_offset % group->file_size == LOG_FILE_HDR_SIZE) {
		/* We start to write a new log file instance in the group */

		ut_a(next_offset / group->file_size <= ULINT_MAX);

		log_group_file_header_flush(group, (ulint)
					    (next_offset / group->file_size),
					    start_lsn);
		srv_stats.os_log_written.add(OS_FILE_LOG_BLOCK_SIZE);

		srv_stats.log_writes.inc();
	}

	if ((next_offset % group->file_size) + len > group->file_size) {

		/* if the above condition holds, then the below expression
		is < len which is ulint, so the typecast is ok */
		write_len = (ulint)
			(group->file_size - (next_offset % group->file_size));
	} else {
		write_len = len;
	}

	DBUG_PRINT("ib_log",
		   ("write " LSN_PF " to " LSN_PF
		    ": group " ULINTPF " len " ULINTPF
		    " blocks " ULINTPF ".." ULINTPF,
		    start_lsn, next_offset,
		    group->id, write_len,
		    log_block_get_hdr_no(buf),
		    log_block_get_hdr_no(
			    buf + write_len
			    - OS_FILE_LOG_BLOCK_SIZE)));

	ut_ad(pad_len >= len
	      || log_block_get_hdr_no(buf)
		 == log_block_convert_lsn_to_no(start_lsn));

	/* Calculate the checksums for each log block and write them to
	the trailer fields of the log blocks */

	for (i = 0; i < write_len / OS_FILE_LOG_BLOCK_SIZE; i++) {
		ut_ad(pad_len >= len
		      || i * OS_FILE_LOG_BLOCK_SIZE >= len - pad_len
		      || log_block_get_hdr_no(
			      buf + i * OS_FILE_LOG_BLOCK_SIZE)
			 == log_block_get_hdr_no(buf) + i);
		log_block_store_checksum(buf + i * OS_FILE_LOG_BLOCK_SIZE);
	}

	log_sys->n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	srv_stats.os_log_pending_writes.inc();

	ut_a(next_offset / UNIV_PAGE_SIZE <= ULINT_MAX);

	const ulint	page_no
		= (ulint) (next_offset / univ_page_size.physical());

	fil_io(OS_FILE_WRITE | OS_FILE_LOG, true,
	       page_id_t(group->space_id, page_no),
	       univ_page_size,
	       (ulint) (next_offset % UNIV_PAGE_SIZE), write_len, buf,
	       group);

	srv_stats.os_log_pending_writes.dec();

	srv_stats.os_log_written.add(write_len);
	srv_stats.log_writes.inc();

	if (write_len < len) {
		start_lsn += write_len;
		len -= write_len;
		buf += write_len;

		write_header = true;

		goto loop;
	}
}

/** Ensure that the log has been written to the log file up to a given
log entry (such as that of a transaction commit). Start a new write, or
wait and check if an already running write is covering the request.
@param[in]	lsn		log sequence number that should be
included in the redo log file write
@param[in]	flush_to_disk	whether the written log should also
be flushed to the file system */

void
log_write_up_to(
	lsn_t	lsn,
	bool	flush_to_disk)
{
#ifdef UNIV_DEBUG
	ulint		loop_count	= 0;
#endif /* UNIV_DEBUG */

	ut_ad(!srv_read_only_mode);

	if (recv_no_ibuf_operations) {
		/* Recovery is running and no operations on the log files are
		allowed yet (the variable name .._no_ibuf_.. is misleading) */

		return;
	}

loop:
	ut_ad(++loop_count < 128);

#if UNIV_WORD_SIZE > 7
	/* We can do a dirty read of LSN. */
	/* NOTE: Currently doesn't do dirty read for
	(flush_to_disk == true) case, because the log_mutex
	contention also works as the arbitrator for write-IO
	(fsync) bandwidth between log files and data files. */
	os_rmb;
	if (!flush_to_disk && log_sys->write_lsn >= lsn) {
		return;
	}
#endif

	log_mutex_enter();
	ut_ad(!recv_no_log_write);

	lsn_t	limit_lsn = flush_to_disk
		? log_sys->flushed_to_disk_lsn
		: log_sys->write_lsn;

	if (limit_lsn >= lsn) {
		log_mutex_exit();
		return;
	}

#ifdef _WIN32
	/* write requests during fil_flush() might not be good for Windows */
	if (log_sys->n_pending_flushes > 0
	    || !os_event_is_set(log_sys->flush_event)) {
		log_mutex_exit();
		os_event_wait(log_sys->flush_event);
		goto loop;
	}
#endif /* _WIN32 */

	/* If it is a write call we should just go ahead and do it
	as we checked that write_lsn is not where we'd like it to
	be. If we have to flush as well then we check if there is a
	pending flush and based on that we wait for it to finish
	before proceeding further. */
	if (flush_to_disk
	    && (log_sys->n_pending_flushes > 0
		|| !os_event_is_set(log_sys->flush_event))) {
		/* Figure out if the current flush will do the job
		for us. */
		bool work_done = log_sys->current_flush_lsn >= lsn;

		log_mutex_exit();

		os_event_wait(log_sys->flush_event);

		if (work_done) {
			return;
		} else {
			goto loop;
		}
	}

	if (!flush_to_disk
	    && log_sys->buf_free == log_sys->buf_next_to_write) {
		/* Nothing to write and no flush to disk requested */
		log_mutex_exit();
		return;
	}

	log_group_t*	group;
	ulint		start_offset;
	ulint		end_offset;
	ulint		area_start;
	ulint		area_end;
	ulong		write_ahead_size = srv_log_write_ahead_size;
	ulint		pad_size;

	DBUG_PRINT("ib_log", ("write " LSN_PF " to " LSN_PF,
			      log_sys->write_lsn,
			      log_sys->lsn));

	if (flush_to_disk) {
		log_sys->n_pending_flushes++;
		log_sys->current_flush_lsn = log_sys->lsn;
		MONITOR_INC(MONITOR_PENDING_LOG_FLUSH);
		os_event_reset(log_sys->flush_event);
	}

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	start_offset = log_sys->buf_next_to_write;
	end_offset = log_sys->buf_free;

	area_start = ut_calc_align_down(start_offset, OS_FILE_LOG_BLOCK_SIZE);
	area_end = ut_calc_align(end_offset, OS_FILE_LOG_BLOCK_SIZE);

	ut_ad(area_end - area_start > 0);

	log_block_set_flush_bit(log_sys->buf + area_start, TRUE);
	log_block_set_checkpoint_no(
		log_sys->buf + area_end - OS_FILE_LOG_BLOCK_SIZE,
		log_sys->next_checkpoint_no);

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	/* Calculate pad_size if needed. */
	pad_size = 0;
	if (write_ahead_size > OS_FILE_LOG_BLOCK_SIZE) {
		lsn_t	end_offset;
		ulint	end_offset_in_unit;

		end_offset = log_group_calc_lsn_offset(
			ut_uint64_align_up(log_sys->lsn,
					   OS_FILE_LOG_BLOCK_SIZE),
			group);
		end_offset_in_unit = (ulint) (end_offset % write_ahead_size);

		if (end_offset_in_unit > 0
		    && (area_end - area_start) > end_offset_in_unit) {
			/* The first block in the unit was initialized
			after the last writing.
			Needs to be written padded data once. */
			pad_size = write_ahead_size - end_offset_in_unit;

			if (area_end + pad_size > log_sys->buf_size) {
				pad_size = log_sys->buf_size - area_end;
			}

			::memset(log_sys->buf + area_end, 0, pad_size);
		}
	}

	/* Do the write to the log files */
	log_group_write_buf(
		group, log_sys->buf + area_start,
		area_end - area_start + pad_size,
#ifdef UNIV_DEBUG
		pad_size,
#endif /* UNIV_DEBUG */
		ut_uint64_align_down(log_sys->write_lsn,
				     OS_FILE_LOG_BLOCK_SIZE),
		start_offset - area_start);

	srv_stats.log_padded.add(pad_size);

	log_sys->write_end_offset = log_sys->buf_free;

	log_group_set_fields(group, log_sys->write_lsn);

	log_sys_write_completion();

#ifndef _WIN32
	if (srv_unix_file_flush_method == SRV_UNIX_O_DSYNC
	    || srv_unix_file_flush_method == SRV_UNIX_ALL_O_DIRECT) {
		/* O_SYNC and ALL_O_DIRECT mean the OS did not buffer the log
		file at all: so we have also flushed to disk what we have
		written */
		log_sys->flushed_to_disk_lsn = log_sys->write_lsn;
	}
#endif /* !_WIN32 */

	log_mutex_exit();

	if (!flush_to_disk) {
		/* Only write requested. */
		return;
	}

	ut_a(log_sys->n_pending_flushes == 1); /* No other threads here */

#ifndef _WIN32
	bool	do_flush = srv_unix_file_flush_method != SRV_UNIX_O_DSYNC;
#else
	bool	do_flush = true;
#endif
	if (do_flush) {
		group = UT_LIST_GET_FIRST(log_sys->log_groups);
		fil_flush(group->space_id);
		log_sys->flushed_to_disk_lsn = log_sys->current_flush_lsn;
	}

	log_sys->n_pending_flushes--;
	MONITOR_DEC(MONITOR_PENDING_LOG_FLUSH);

	os_event_set(log_sys->flush_event);
}

/****************************************************************//**
Does a syncronous flush of the log buffer to disk. */

void
log_buffer_flush_to_disk(void)
/*==========================*/
{
	lsn_t	lsn;

	ut_ad(!srv_read_only_mode);
	log_mutex_enter();

	lsn = log_sys->lsn;

	log_mutex_exit();

	log_write_up_to(lsn, true);
}

/****************************************************************//**
This functions writes the log buffer to the log file and if 'flush'
is set it forces a flush of the log file as well. This is meant to be
called from background master thread only as it does not wait for
the write (+ possible flush) to finish. */

void
log_buffer_sync_in_background(
/*==========================*/
	bool	flush)	/*!< in: flush the logs to disk */
{
	lsn_t	lsn;

	log_mutex_enter();

	lsn = log_sys->lsn;

	if (flush
	    && log_sys->n_pending_flushes > 0
	    && log_sys->current_flush_lsn >= lsn) {
		/* The write + flush will write enough */
		log_mutex_exit();
		return;
	}

	log_mutex_exit();

	log_write_up_to(lsn, flush);
}

/********************************************************************

Tries to establish a big enough margin of free space in the log buffer, such
that a new log entry can be catenated without an immediate need for a flush. */
static
void
log_flush_margin(void)
/*==================*/
{
	log_t*	log	= log_sys;
	lsn_t	lsn	= 0;

	log_mutex_enter();

	if (log->buf_free > log->max_buf_free) {
		/* We can write during flush */
		lsn = log->lsn;
	}

	log_mutex_exit();

	if (lsn) {
		log_write_up_to(lsn, false);
	}
}

/****************************************************************//**
Advances the smallest lsn for which there are unflushed dirty blocks in the
buffer pool. NOTE: this function may only be called if the calling thread owns
no synchronization objects!
@return false if there was a flush batch of the same type running,
which means that we could not start this flush batch */
static
bool
log_preflush_pool_modified_pages(
/*=============================*/
	lsn_t	new_oldest)	/*!< in: try to advance oldest_modified_lsn
				at least to this lsn */
{
	lsn_t	current_oldest;
	ulint	i;

	if (recv_recovery_on) {
		/* If the recovery is running, we must first apply all
		log records to their respective file pages to get the
		right modify lsn values to these pages: otherwise, there
		might be pages on disk which are not yet recovered to the
		current lsn, and even after calling this function, we could
		not know how up-to-date the disk version of the database is,
		and we could not make a new checkpoint on the basis of the
		info on the buffer pool only. */

		recv_apply_hashed_log_recs(TRUE);
	}

	if (!buf_page_cleaner_is_active
	    || (srv_foreground_preflush
		== SRV_FOREGROUND_PREFLUSH_SYNC_PREFLUSH)
	    || (new_oldest == LSN_MAX)) {

		ulint n_pages;

		bool success = buf_flush_lists(ULINT_MAX, new_oldest, &n_pages);

		buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);

		if (!success) {
			MONITOR_INC(MONITOR_FLUSH_SYNC_WAITS);
		}

		MONITOR_INC_VALUE_CUMULATIVE(
			MONITOR_FLUSH_SYNC_TOTAL_PAGE,
			MONITOR_FLUSH_SYNC_COUNT,
			MONITOR_FLUSH_SYNC_PAGES,
			n_pages);

		return(success);
	}

	ut_ad(srv_foreground_preflush == SRV_FOREGROUND_PREFLUSH_EXP_BACKOFF);

	current_oldest = buf_pool_get_oldest_modification();
	i = 0;

	while (current_oldest < new_oldest && current_oldest) {

		while (!buf_flush_flush_list_in_progress()) {

			/* If a flush list flush by the cleaner thread is not
			running, backoff until one is started.  */
			os_thread_sleep(ut_rnd_interval(0, 1 << i));
			i++;
			i %= 16;
		}
		buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);

		current_oldest = buf_pool_get_oldest_modification();
	}

	return(current_oldest >= new_oldest || !current_oldest);
}

/******************************************************//**
Completes a checkpoint. */
static
void
log_complete_checkpoint(void)
/*=========================*/
{
	ut_ad(log_mutex_own());
	ut_ad(log_sys->n_pending_checkpoint_writes == 0);

	log_sys->next_checkpoint_no++;

	log_sys->last_checkpoint_lsn = log_sys->next_checkpoint_lsn;
	MONITOR_SET(MONITOR_LSN_CHECKPOINT_AGE,
		    log_sys->lsn - log_sys->last_checkpoint_lsn);

	DBUG_PRINT("ib_log", ("checkpoint ended at " LSN_PF
			      ", flushed to " LSN_PF,
			      log_sys->last_checkpoint_lsn,
			      log_sys->flushed_to_disk_lsn));

	rw_lock_x_unlock_gen(&(log_sys->checkpoint_lock), LOG_CHECKPOINT);
}

/******************************************************//**
Completes an asynchronous checkpoint info write i/o to a log file. */
static
void
log_io_complete_checkpoint(void)
/*============================*/
{
	MONITOR_DEC(MONITOR_PENDING_CHECKPOINT_WRITE);

	log_mutex_enter();

	ut_ad(log_sys->n_pending_checkpoint_writes > 0);

	if (--log_sys->n_pending_checkpoint_writes == 0) {
		log_complete_checkpoint();
	}

	log_mutex_exit();

	/* Wake the redo log watching thread to parse the log up to this
	checkpoint. */
	if (srv_track_changed_pages) {
		os_event_reset(srv_redo_log_tracked_event);
		os_event_set(srv_checkpoint_completed_event);
	}
}

/*******************************************************************//**
Writes info to a checkpoint about a log group. */
static
void
log_checkpoint_set_nth_group_info(
/*==============================*/
	byte*	buf,	/*!< in: buffer for checkpoint info */
	ulint	n,	/*!< in: nth slot */
	lsn_t	file_no)/*!< in: archived file number */
{
	ut_ad(n < LOG_MAX_N_GROUPS);

	mach_write_to_8(buf + LOG_CHECKPOINT_GROUP_ARRAY +
			8 * n + LOG_CHECKPOINT_ARCHIVED_FILE_NO,
			file_no);
}

/*******************************************************************//**
Gets info from a checkpoint about a log group. */

void
log_checkpoint_get_nth_group_info(
/*==============================*/
	const byte*	buf,	/*!< in: buffer containing checkpoint info */
	ulint		n,	/*!< in: nth slot */
	lsn_t*		file_no)/*!< out: archived file number */
{
	ut_ad(n < LOG_MAX_N_GROUPS);

	*file_no = mach_read_from_8(buf + LOG_CHECKPOINT_GROUP_ARRAY +
				8 * n + LOG_CHECKPOINT_ARCHIVED_FILE_NO);
}

/******************************************************//**
Writes the checkpoint info to a log group header. */
static
void
log_group_checkpoint(
/*=================*/
	log_group_t*	group)	/*!< in: log group */
{
	lsn_t	archived_lsn;
	lsn_t	lsn_offset;
	ulint	fold;
	byte*	buf;
	ulint	i;

	ut_ad(!srv_read_only_mode);
	ut_ad(log_mutex_own());
#if LOG_CHECKPOINT_SIZE > OS_FILE_LOG_BLOCK_SIZE
# error "LOG_CHECKPOINT_SIZE > OS_FILE_LOG_BLOCK_SIZE"
#endif

	DBUG_PRINT("ib_log", ("checkpoint " UINT64PF " at " LSN_PF
			      " written to group " ULINTPF,
			      log_sys->next_checkpoint_no,
			      log_sys->next_checkpoint_lsn,
			      group->id));

	buf = group->checkpoint_buf;

	mach_write_to_8(buf + LOG_CHECKPOINT_NO, log_sys->next_checkpoint_no);
	mach_write_to_8(buf + LOG_CHECKPOINT_LSN, log_sys->next_checkpoint_lsn);

	lsn_offset = log_group_calc_lsn_offset(log_sys->next_checkpoint_lsn,
					       group);
	mach_write_to_4(buf + LOG_CHECKPOINT_OFFSET_LOW32,
			lsn_offset & 0xFFFFFFFFUL);
	mach_write_to_4(buf + LOG_CHECKPOINT_OFFSET_HIGH32,
			lsn_offset >> 32);

	mach_write_to_4(buf + LOG_CHECKPOINT_LOG_BUF_SIZE, log_sys->buf_size);

	if (log_sys->archiving_state == LOG_ARCH_OFF) {
		archived_lsn = LSN_MAX;
	} else {
		archived_lsn = log_sys->archived_lsn;
	}

	mach_write_to_8(buf + LOG_CHECKPOINT_ARCHIVED_LSN, archived_lsn);

	for (i = 0; i < LOG_MAX_N_GROUPS; i++) {
		log_checkpoint_set_nth_group_info(buf, i, 0);
	}

	for (log_group_t* group2 = UT_LIST_GET_FIRST(log_sys->log_groups);
	     group2 != NULL;
	     group2 = UT_LIST_GET_NEXT(log_groups, group2)) {
		log_checkpoint_set_nth_group_info(buf, group2->id,
						  group2->archived_file_no);
	}

	fold = ut_fold_binary(buf, LOG_CHECKPOINT_CHECKSUM_1);
	mach_write_to_4(buf + LOG_CHECKPOINT_CHECKSUM_1, fold);

	fold = ut_fold_binary(buf + LOG_CHECKPOINT_LSN,
			      LOG_CHECKPOINT_CHECKSUM_2 - LOG_CHECKPOINT_LSN);
	mach_write_to_4(buf + LOG_CHECKPOINT_CHECKSUM_2, fold);

	MONITOR_INC(MONITOR_PENDING_CHECKPOINT_WRITE);

	log_sys->n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	ut_ad(LOG_CHECKPOINT_1 < univ_page_size.physical());
	ut_ad(LOG_CHECKPOINT_2 < univ_page_size.physical());

	if (log_sys->n_pending_checkpoint_writes++ == 0) {
		rw_lock_x_lock_gen(&log_sys->checkpoint_lock,
				   LOG_CHECKPOINT);
	}

	/* We send as the last parameter the group machine address
	added with 1, as we want to distinguish between a normal log
	file write and a checkpoint field write */

	fil_io(OS_FILE_WRITE | OS_FILE_LOG, false,
	       page_id_t(group->space_id, 0),
	       univ_page_size,
	       log_sys->next_checkpoint_no & 1
	       ? LOG_CHECKPOINT_2 : LOG_CHECKPOINT_1,
	       OS_FILE_LOG_BLOCK_SIZE,
	       buf, (byte*) group + 1);

	ut_ad(((ulint) group & 0x1UL) == 0);
}
#endif /* !UNIV_HOTBACKUP */

#ifdef UNIV_HOTBACKUP
/******************************************************//**
Writes info to a buffer of a log group when log files are created in
backup restoration. */

void
log_reset_first_header_and_checkpoint(
/*==================================*/
	byte*		hdr_buf,/*!< in: buffer which will be written to the
				start of the first log file */
	ib_uint64_t	start)	/*!< in: lsn of the start of the first log file;
				we pretend that there is a checkpoint at
				start + LOG_BLOCK_HDR_SIZE */
{
	ulint		fold;
	byte*		buf;
	ib_uint64_t	lsn;

	mach_write_to_4(hdr_buf + LOG_GROUP_ID, 0);
	mach_write_to_8(hdr_buf + LOG_FILE_START_LSN, start);

	lsn = start + LOG_BLOCK_HDR_SIZE;

	/* Write the label of mysqlbackup --restore */
	strcpy((char*) hdr_buf + LOG_FILE_WAS_CREATED_BY_HOT_BACKUP,
	       "ibbackup ");
	ut_sprintf_timestamp((char*) hdr_buf
			     + (LOG_FILE_WAS_CREATED_BY_HOT_BACKUP
				+ (sizeof "ibbackup ") - 1));
	buf = hdr_buf + LOG_CHECKPOINT_1;

	mach_write_to_8(buf + LOG_CHECKPOINT_NO, 0);
	mach_write_to_8(buf + LOG_CHECKPOINT_LSN, lsn);

	mach_write_to_4(buf + LOG_CHECKPOINT_OFFSET_LOW32,
			LOG_FILE_HDR_SIZE + LOG_BLOCK_HDR_SIZE);
	mach_write_to_4(buf + LOG_CHECKPOINT_OFFSET_HIGH32, 0);

	mach_write_to_4(buf + LOG_CHECKPOINT_LOG_BUF_SIZE, 2 * 1024 * 1024);

	mach_write_to_8(buf + LOG_CHECKPOINT_ARCHIVED_LSN, LSN_MAX);

	fold = ut_fold_binary(buf, LOG_CHECKPOINT_CHECKSUM_1);
	mach_write_to_4(buf + LOG_CHECKPOINT_CHECKSUM_1, fold);

	fold = ut_fold_binary(buf + LOG_CHECKPOINT_LSN,
			      LOG_CHECKPOINT_CHECKSUM_2 - LOG_CHECKPOINT_LSN);
	mach_write_to_4(buf + LOG_CHECKPOINT_CHECKSUM_2, fold);

	/* Starting from InnoDB-3.23.50, we should also write info on
	allocated size in the tablespace, but unfortunately we do not
	know it here */
}
#endif /* UNIV_HOTBACKUP */

#ifndef UNIV_HOTBACKUP
/******************************************************//**
Reads a checkpoint info from a log group header to log_sys->checkpoint_buf. */

void
log_group_read_checkpoint_info(
/*===========================*/
	log_group_t*	group,	/*!< in: log group */
	ulint		field)	/*!< in: LOG_CHECKPOINT_1 or LOG_CHECKPOINT_2 */
{
	ut_ad(log_mutex_own());

	log_sys->n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	fil_io(OS_FILE_READ | OS_FILE_LOG, true,
	       page_id_t(group->space_id, field / univ_page_size.physical()),
	       univ_page_size, field % univ_page_size.physical(),
	       OS_FILE_LOG_BLOCK_SIZE, log_sys->checkpoint_buf, NULL);
}

/** Write checkpoint info to the log header and invoke log_mutex_exit().
@param[in]	sync	whether to wait for the write to complete */

void
log_write_checkpoint_info(
	bool	sync)
{
	log_group_t*	group;

	ut_ad(log_mutex_own());

	if (!srv_read_only_mode) {
		for (group = UT_LIST_GET_FIRST(log_sys->log_groups);
		     group;
		     group = UT_LIST_GET_NEXT(log_groups, group)) {

			log_group_checkpoint(group);
		}
	}

	log_mutex_exit();

	MONITOR_INC(MONITOR_NUM_CHECKPOINT);

	if (sync) {
		/* Wait for the checkpoint write to complete */
		rw_lock_s_lock(&log_sys->checkpoint_lock);
		rw_lock_s_unlock(&log_sys->checkpoint_lock);
	}
}

/** Set extra data to be written to the redo log during checkpoint.
@param[in]	buf	data to be appended on checkpoint, or NULL
@return pointer to previous data to be appended on checkpoint */

mtr_buf_t*
log_append_on_checkpoint(
	mtr_buf_t*	buf)
{
	log_mutex_enter();
	mtr_buf_t*	old = log_sys->append_on_checkpoint;
	log_sys->append_on_checkpoint = buf;
	log_mutex_exit();
	return(old);
}

/** Make a checkpoint. Note that this function does not flush dirty
blocks from the buffer pool: it only checks what is lsn of the oldest
modification in the pool, and writes information about the lsn in
log files. Use log_make_checkpoint_at() to flush also the pool.
@param[in]	sync		whether to wait for the write to complete
@param[in]	write_always	force a write even if no log
has been generated since the latest checkpoint
@return true if success, false if a checkpoint write was already running */

bool
log_checkpoint(
	bool	sync,
	bool	write_always)
{
	lsn_t	oldest_lsn;

	ut_ad(!srv_read_only_mode);

	if (recv_recovery_is_on()) {
		recv_apply_hashed_log_recs(TRUE);
	}

#ifndef _WIN32
	switch (srv_unix_file_flush_method) {
	case SRV_UNIX_NOSYNC:
	case SRV_UNIX_ALL_O_DIRECT:
		break;
	case SRV_UNIX_O_DSYNC:
	case SRV_UNIX_FSYNC:
	case SRV_UNIX_LITTLESYNC:
	case SRV_UNIX_O_DIRECT:
	case SRV_UNIX_O_DIRECT_NO_FSYNC:
		fil_flush_file_spaces(FIL_TYPE_TABLESPACE);
	}
#endif /* !_WIN32 */

	log_mutex_enter();

	ut_ad(!recv_no_log_write);
	oldest_lsn = log_buf_pool_get_oldest_modification();

	/* Because log also contains headers and dummy log records,
	log_buf_pool_get_oldest_modification() will return log_sys->lsn
	if the buffer pool contains no dirty buffers.
	We must make sure that the log is flushed up to that lsn.
	If there are dirty buffers in the buffer pool, then our
	write-ahead-logging algorithm ensures that the log has been
	flushed up to oldest_lsn. */

	if (!write_always
	    && oldest_lsn
	    == log_sys->last_checkpoint_lsn + SIZE_OF_MLOG_CHECKPOINT) {
		/* Do nothing, because nothing was logged (other than
		a MLOG_CHECKPOINT marker) since the previous checkpoint. */
		log_mutex_exit();
		return(true);
	}

	/* Repeat the MLOG_FILE_NAME records after the checkpoint, in
	case some log records between the checkpoint and log_sys->lsn
	need them. Finally, write a MLOG_CHECKPOINT marker. Redo log
	apply expects to see a MLOG_CHECKPOINT after the checkpoint,
	except on clean shutdown, where the log will be empty after
	the checkpoint.

	It is important that we write out the redo log before any
	further dirty pages are flushed to the tablespace files.  At
	this point, because log_mutex_own(), mtr_commit() in other
	threads will be blocked, and no pages can be added to the
	flush lists. */
	lsn_t		flush_lsn	= oldest_lsn;
	const bool	do_write
		= srv_shutdown_state == SRV_SHUTDOWN_NONE
		|| flush_lsn != log_sys->lsn;

	if (fil_names_clear(flush_lsn, do_write)) {
		ut_ad(log_sys->lsn >= flush_lsn + SIZE_OF_MLOG_CHECKPOINT);
		flush_lsn = log_sys->lsn;
	}

	log_mutex_exit();

	log_write_up_to(flush_lsn, true);

	log_mutex_enter();

	if (!write_always
	    && log_sys->last_checkpoint_lsn >= oldest_lsn) {
		log_mutex_exit();
		return(true);
	}

	ut_ad(log_sys->flushed_to_disk_lsn >= oldest_lsn);

	if (log_sys->n_pending_checkpoint_writes > 0) {
		/* A checkpoint write is running */
		log_mutex_exit();

		if (sync) {
			/* Wait for the checkpoint write to complete */
			rw_lock_s_lock(&log_sys->checkpoint_lock);
			rw_lock_s_unlock(&log_sys->checkpoint_lock);
		}

		return(false);
	}

	log_sys->next_checkpoint_lsn = oldest_lsn;
	log_write_checkpoint_info(sync);
	ut_ad(!log_mutex_own());

	return(true);
}

/** Make a checkpoint at or after a specified LSN.
@param[in]	lsn		the log sequence number, or LSN_MAX
for the latest LSN
@param[in]	write_always	force a write even if no log
has been generated since the latest checkpoint */

void
log_make_checkpoint_at(
	lsn_t	lsn,
	bool	write_always)
{
	/* Preflush pages synchronously */

	while (!log_preflush_pool_modified_pages(lsn)) {
		/* Flush as much as we can */
	}

	while (!log_checkpoint(true, write_always)) {
		/* Force a checkpoint */
	}
}

/****************************************************************//**
Tries to establish a big enough margin of free space in the log groups, such
that a new log entry can be catenated without an immediate need for a
checkpoint. NOTE: this function may only be called if the calling thread
owns no synchronization objects! */
static
void
log_checkpoint_margin(void)
/*=======================*/
{
	log_t*		log		= log_sys;
	lsn_t		age;
	lsn_t		checkpoint_age;
	ib_uint64_t	advance;
	lsn_t		oldest_lsn;
	bool		success;
loop:
	advance = 0;

	log_mutex_enter();
	ut_ad(!recv_no_log_write);

	if (!log->check_flush_or_checkpoint) {
		log_mutex_exit();
		return;
	}

	oldest_lsn = log_buf_pool_get_oldest_modification();

	age = log->lsn - oldest_lsn;

	if (age > log->max_modified_age_sync) {

		/* A flush is urgent: we have to do a synchronous preflush */
		advance = 2 * (age - log->max_modified_age_sync);
	}

	checkpoint_age = log->lsn - log->last_checkpoint_lsn;

	bool	checkpoint_sync;
	bool	do_checkpoint;

	if (checkpoint_age > log->max_checkpoint_age) {
		/* A checkpoint is urgent: we do it synchronously */
		checkpoint_sync = true;
		do_checkpoint = true;
	} else if (checkpoint_age > log->max_checkpoint_age_async) {
		/* A checkpoint is not urgent: do it asynchronously */
		do_checkpoint = true;
		checkpoint_sync = false;
		log->check_flush_or_checkpoint = false;
	} else {
		do_checkpoint = false;
		checkpoint_sync = false;
		log->check_flush_or_checkpoint = false;
	}

	log_mutex_exit();

	if (advance) {
		lsn_t	new_oldest = oldest_lsn + advance;

		success = log_preflush_pool_modified_pages(new_oldest);

		/* If the flush succeeded, this thread has done its part
		and can proceed. If it did not succeed, there was another
		thread doing a flush at the same time. */
		if (!success) {
			log_mutex_enter();

			log->check_flush_or_checkpoint = true;

			log_mutex_exit();
			goto loop;
		}
	}

	if (do_checkpoint) {
		log_checkpoint(checkpoint_sync, FALSE);

		if (checkpoint_sync) {

			goto loop;
		}
	}
}

/******************************************************//**
Reads a specified log segment to a buffer.  Optionally releases the log mutex
before the I/O.  */

void
log_group_read_log_seg(
/*===================*/
	ulint		type,		/*!< in: LOG_ARCHIVE or LOG_RECOVER */
	byte*		buf,		/*!< in: buffer where to read */
	log_group_t*	group,		/*!< in: log group */
	lsn_t		start_lsn,	/*!< in: read area start */
	lsn_t		end_lsn,	/*!< in: read area end */
	bool		release_mutex)	/*!< in: whether the log_sys->mutex
					should be released before the read */
{
	ulint	len;
	lsn_t	source_offset;

	ut_ad(log_mutex_own());

	const bool	sync = (type == LOG_RECOVER);
loop:
	source_offset = log_group_calc_lsn_offset(start_lsn, group);

	ut_a(end_lsn - start_lsn <= ULINT_MAX);
	len = (ulint) (end_lsn - start_lsn);

	ut_ad(len != 0);

	if ((source_offset % group->file_size) + len > group->file_size) {

		/* If the above condition is true then len (which is ulint)
		is > the expression below, so the typecast is ok */
		len = (ulint) (group->file_size -
			(source_offset % group->file_size));
	}

	if (type == LOG_ARCHIVE) {

		log_sys->n_pending_archive_ios++;
	}

	log_sys->n_log_ios++;

	MONITOR_INC(MONITOR_LOG_IO);

	ut_a(source_offset / UNIV_PAGE_SIZE <= ULINT_MAX);

	if (release_mutex) {
		log_mutex_exit();
	}

	const ulint	page_no
		= (ulint) (source_offset / univ_page_size.physical());

	fil_io(OS_FILE_READ | OS_FILE_LOG, sync,
	       page_id_t(group->space_id, page_no),
	       univ_page_size,
	       (ulint) (source_offset % univ_page_size.physical()),
	       len, buf, (type == LOG_ARCHIVE) ? &log_archive_io : NULL);

	start_lsn += len;
	buf += len;

	if (start_lsn != end_lsn) {

		if (release_mutex) {
			log_mutex_enter();
		}
		goto loop;
	}
}

/**
Checks that there is enough free space in the log to start a new query step.
Flushes the log buffer or makes a new checkpoint if necessary. NOTE: this
function may only be called if the calling thread owns no synchronization
objects! */

void
log_check_margins(void)
{
	bool	check	= true;

	do {
		log_flush_margin();
		log_checkpoint_margin();
		log_mutex_enter();
		if (log_check_tracking_margin(0)) {
			log_mutex_exit();
			os_thread_sleep(10000);
			continue;
		}
		log_mutex_exit();
		log_archive_margin();
		log_mutex_enter();
		ut_ad(!recv_no_log_write);
		check = log_sys->check_flush_or_checkpoint;
		log_mutex_exit();
	} while (check);
}

/****************************************************************//**
Makes a checkpoint at the latest lsn and writes it to first page of each
data file in the database, so that we know that the file spaces contain
all modifications up to that lsn. This can only be called at database
shutdown. This function also writes all log in log files to the log archive. */

void
logs_empty_and_mark_files_at_shutdown(void)
/*=======================================*/
{
	lsn_t			lsn;
	lsn_t			tracked_lsn;
	ulint			count = 0;
	ulint			total_trx;
	ulint			pending_io;
	enum srv_thread_type	active_thd;
	const char*		thread_name;

	ib_logf(IB_LOG_LEVEL_INFO, "Starting shutdown...");

	/* Wait until the master thread and all other operations are idle: our
	algorithm only works if the server is idle at shutdown */

	srv_shutdown_state = SRV_SHUTDOWN_CLEANUP;
loop:
	os_thread_sleep(100000);

	count++;

	/* We need the monitor threads to stop before we proceed with
	a shutdown. */

	thread_name = srv_any_background_threads_are_active();

	if (thread_name != NULL) {
		/* Print a message every 60 seconds if we are waiting
		for the monitor thread to exit. Master and worker
		threads check will be done later. */

		if (srv_print_verbose_log && count > 600) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Waiting for %s to exit", thread_name);
			count = 0;
		}

		goto loop;
	}

	/* Check that there are no longer transactions, except for
	PREPARED ones. We need this wait even for the 'very fast'
	shutdown, because the InnoDB layer may have committed or
	prepared transactions and we don't want to lose them. */

	total_trx = trx_sys_any_active_transactions();

	if (total_trx > 0) {

		if (srv_print_verbose_log && count > 600) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Waiting for %lu active transactions to finish",
				(ulong) total_trx);

			count = 0;
		}

		goto loop;
	}

	/* Check that the background threads are suspended */

	active_thd = srv_get_active_thread_type();

	if (active_thd != SRV_NONE) {

		if (active_thd == SRV_PURGE) {
			srv_purge_wakeup();
		}

		/* The srv_lock_timeout_thread, srv_error_monitor_thread
		and srv_monitor_thread should already exit by now. The
		only threads to be suspended are the master threads
		and worker threads (purge threads). Print the thread
		type if any of such threads not in suspended mode */
		if (srv_print_verbose_log && count > 600) {
			const char*	thread_type = "<null>";

			switch (active_thd) {
			case SRV_NONE:
				/* This shouldn't happen because we've
				already checked for this case before
				entering the if(). We handle it here
				to avoid a compiler warning. */
				ut_error;
			case SRV_WORKER:
				thread_type = "worker threads";
				break;
			case SRV_MASTER:
				thread_type = "master thread";
				break;
			case SRV_PURGE:
				thread_type = "purge thread";
				break;
			}

			ib_logf(IB_LOG_LEVEL_INFO,
				"Waiting for %s to be suspended",
				thread_type);
			count = 0;
		}

		goto loop;
	}

	/* At this point only page_cleaner should be active. We wait
	here to let it complete the flushing of the buffer pools
	before proceeding further. */
	srv_shutdown_state = SRV_SHUTDOWN_FLUSH_PHASE;
	count = 0;
	while (buf_page_cleaner_is_active) {
		++count;
		os_thread_sleep(100000);
		if (srv_print_verbose_log && count > 600) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Waiting for page_cleaner to"
				" finish flushing of buffer pool");
			count = 0;
		}
	}

	log_mutex_enter();
	const ulint	n_write	= log_sys->n_pending_checkpoint_writes;
	const ulint	n_flush	= log_sys->n_pending_flushes;
	log_mutex_exit();

	if (n_write != 0 || n_flush != 0) {
		if (srv_print_verbose_log && count > 600) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Pending checkpoint_writes: " ULINTPF "."
				" Pending log flush writes: " ULINTPF,
				n_write, n_flush);
			count = 0;
		}
		goto loop;
	}

	pending_io = buf_pool_check_no_pending_io();

	if (pending_io) {
		if (srv_print_verbose_log && count > 600) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Waiting for %lu buffer page I/Os to complete",
				(ulong) pending_io);
			count = 0;
		}

		goto loop;
	}

	if (srv_fast_shutdown == 2) {
		if (!srv_read_only_mode) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"MySQL has requested a very fast shutdown"
				" without flushing the InnoDB buffer pool to"
				" data files. At the next mysqld startup"
				" InnoDB will do a crash recovery!");

			/* In this fastest shutdown we do not flush the
			buffer pool:

			it is essentially a 'crash' of the InnoDB server.
			Make sure that the log is all flushed to disk, so
			that we can recover all committed transactions in
			a crash recovery. We must not write the lsn stamps
			to the data files, since at a startup InnoDB deduces
			from the stamps if the previous shutdown was clean. */

			log_buffer_flush_to_disk();

			/* Check that the background threads stay suspended */
			thread_name = srv_any_background_threads_are_active();

			if (thread_name != NULL) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"Background thread %s woke up"
					" during shutdown", thread_name);
				goto loop;
			}
		}

		srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;

		/* Wake the log tracking thread which will then immediatelly
		quit because of srv_shutdown_state value */
		if (srv_track_changed_pages) {
			os_event_reset(srv_redo_log_tracked_event);
			os_event_set(srv_checkpoint_completed_event);
		}

		fil_close_all_files();

		thread_name = srv_any_background_threads_are_active();

		ut_a(!thread_name);

		return;
	}

	if (!srv_read_only_mode) {
		log_make_checkpoint_at(LSN_MAX, TRUE);
	}

	log_mutex_enter();

	tracked_lsn = log_get_tracked_lsn();

	lsn = log_sys->lsn;
	const bool	is_last	= (lsn == log_sys->last_checkpoint_lsn)
		&& (!srv_track_changed_pages
		    || tracked_lsn == log_sys->last_checkpoint_lsn)
		&& (!srv_log_archive_on
		    || lsn == log_sys->archived_lsn + LOG_BLOCK_HDR_SIZE);
	ut_ad(lsn >= log_sys->last_checkpoint_lsn);

	log_mutex_exit();

	if (!is_last) {
		goto loop;
	}

	log_mutex_enter();
	log_archive_close_groups(TRUE);
	log_mutex_exit();

	/* Check that the background threads stay suspended */
	thread_name = srv_any_background_threads_are_active();
	if (thread_name != NULL) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Background thread %s woke up during shutdown",
			thread_name);

		goto loop;
	}

	if (!srv_read_only_mode) {
		fil_flush_file_spaces(FIL_TYPE_TABLESPACE);
		fil_flush_file_spaces(FIL_TYPE_LOG);
	}

	/* The call fil_write_flushed_lsn() will bypass the buffer
	pool: therefore it is essential that the buffer pool has been
	completely flushed to disk! (We do not call fil_write... if the
	'very fast' shutdown is enabled.) */

	if (!buf_all_freed()) {

		if (srv_print_verbose_log && count > 600) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Waiting for dirty buffer pages to be flushed");
			count = 0;
		}

		goto loop;
	}

	srv_shutdown_state = SRV_SHUTDOWN_LAST_PHASE;

	/* Signal the log following thread to quit */
	if (srv_track_changed_pages) {
		os_event_reset(srv_redo_log_tracked_event);
		os_event_set(srv_checkpoint_completed_event);
	}

	/* Make some checks that the server really is quiet */
	srv_thread_type	type = srv_get_active_thread_type();
	ut_a(type == SRV_NONE);

	bool	freed = buf_all_freed();
	ut_a(freed);

	ut_a(lsn == log_sys->lsn);

	if (lsn < srv_start_lsn) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Log sequence number at shutdown " LSN_PF
			" is lower than at startup " LSN_PF "!",
			lsn, srv_start_lsn);
	}

	srv_shutdown_lsn = lsn;

	if (!srv_read_only_mode) {
		fil_write_flushed_lsn(lsn);
	}

	fil_close_all_files();

	/* Make some checks that the server really is quiet */
	type = srv_get_active_thread_type();
	ut_a(type == SRV_NONE);

	freed = buf_all_freed();
	ut_a(freed);

	ut_a(lsn == log_sys->lsn);
}

/******************************************************//**
Peeks the current lsn.
@return TRUE if success, FALSE if could not get the log system mutex */

ibool
log_peek_lsn(
/*=========*/
	lsn_t*	lsn)	/*!< out: if returns TRUE, current lsn is here */
{
	if (0 == mutex_enter_nowait(&(log_sys->mutex))) {
		*lsn = log_sys->lsn;

		log_mutex_exit();

		return(TRUE);
	}

	return(FALSE);
}

/******************************************************//**
Prints info of the log. */

void
log_print(
/*======*/
	FILE*	file)	/*!< in: file where to print */
{
	double	time_elapsed;
	time_t	current_time;

	log_mutex_enter();

	fprintf(file,
		"Log sequence number " LSN_PF "\n"
		"Log flushed up to   " LSN_PF "\n"
		"Pages flushed up to " LSN_PF "\n"
		"Last checkpoint at  " LSN_PF "\n",
		log_sys->lsn,
		log_sys->flushed_to_disk_lsn,
		log_buf_pool_get_oldest_modification(),
		log_sys->last_checkpoint_lsn);

	fprintf(file,
		"Max checkpoint age    " LSN_PF "\n"
		"Checkpoint age target " LSN_PF "\n"
		"Modified age          " LSN_PF "\n"
		"Checkpoint age        " LSN_PF "\n",
		log_sys->max_checkpoint_age,
		log_sys->max_checkpoint_age_async,
		log_sys->lsn -log_buf_pool_get_oldest_modification(),
		log_sys->lsn - log_sys->last_checkpoint_lsn);

	current_time = time(NULL);

	time_elapsed = difftime(current_time,
				log_sys->last_printout_time);

	if (time_elapsed <= 0) {
		time_elapsed = 1;
	}

	fprintf(file,
		ULINTPF " pending log flushes, "
		ULINTPF " pending chkp writes\n"
		ULINTPF " log i/o's done, %.2f log i/o's/second\n",
		log_sys->n_pending_flushes,
		log_sys->n_pending_checkpoint_writes,
		log_sys->n_log_ios,
		static_cast<double>(
			log_sys->n_log_ios - log_sys->n_log_ios_old)
		/ time_elapsed);

	if (srv_track_changed_pages) {

		/* The maximum tracked LSN age is equal to the maximum
		checkpoint age */
		fprintf(file,
			"Log tracking enabled\n"
			"Log tracked up to   " LSN_PF "\n"
			"Max tracked LSN age " LSN_PF "\n",
			log_get_tracked_lsn(),
			log_sys->max_checkpoint_age);
	}

	log_sys->n_log_ios_old = log_sys->n_log_ios;
	log_sys->last_printout_time = current_time;

	log_mutex_exit();
}

/**********************************************************************//**
Refreshes the statistics used to print per-second averages. */

void
log_refresh_stats(void)
/*===================*/
{
	log_sys->n_log_ios_old = log_sys->n_log_ios;
	log_sys->last_printout_time = time(NULL);
}

/********************************************************//**
Closes a log group. */
static
void
log_group_close(
/*===========*/
	log_group_t*	group)		/* in,own: log group to close */
{
	ulint	i;

	for (i = 0; i < group->n_files; i++) {
		ut_free(group->file_header_bufs_ptr[i]);
		ut_free(group->archive_file_header_bufs_ptr[i]);
	}

	ut_free(group->file_header_bufs_ptr);
	ut_free(group->file_header_bufs);
	ut_free(group->archive_file_header_bufs_ptr);
	ut_free(group->archive_file_header_bufs);
	ut_free(group->checkpoint_buf_ptr);
	ut_free(group);
}

/********************************************************//**
Closes all log groups. */

void
log_group_close_all(void)
/*=====================*/
{
	log_group_t*	group;

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	while (UT_LIST_GET_LEN(log_sys->log_groups) > 0) {
		log_group_t*	prev_group = group;

		group = UT_LIST_GET_NEXT(log_groups, group);

		UT_LIST_REMOVE(log_sys->log_groups, prev_group);

		log_group_close(prev_group);
	}
}

/********************************************************//**
Shutdown the log system but do not release all the memory. */

void
log_shutdown(void)
/*==============*/
{
	log_group_close_all();

	ut_free(log_sys->buf_ptr);
	log_sys->buf_ptr = NULL;
	log_sys->buf = NULL;
	ut_free(log_sys->checkpoint_buf_ptr);
	log_sys->checkpoint_buf_ptr = NULL;
	log_sys->checkpoint_buf = NULL;
	ut_free(log_sys->archive_buf_ptr);
	log_sys->archive_buf_ptr = NULL;
	log_sys->archive_buf = NULL;

	os_event_destroy(log_sys->flush_event);

	rw_lock_free(&log_sys->checkpoint_lock);

	mutex_free(&log_sys->mutex);
	mutex_free(&log_sys->log_flush_order_mutex);

	rw_lock_free(&log_sys->archive_lock);
	os_event_destroy(log_sys->archiving_on);

	recv_sys_close();
}

/********************************************************//**
Free the log system data structures. */

void
log_mem_free(void)
/*==============*/
{
	if (log_sys != NULL) {
		recv_sys_mem_free();
		ut_free(log_sys);

		log_sys = NULL;
	}
}
#endif /* !UNIV_HOTBACKUP */
