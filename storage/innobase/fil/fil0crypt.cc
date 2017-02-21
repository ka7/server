/*****************************************************************************
Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (c) 2014, 2017, MariaDB Corporation. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

*****************************************************************************/
/**************************************************//**
@file fil0crypt.cc
Innodb file space encrypt/decrypt

Created            Jonas Oreland Google
Modified           Jan Lindström jan.lindstrom@mariadb.com
*******************************************************/

#include "fil0fil.h"
#include "fil0crypt.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "mach0data.h"
#include "log0recv.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "page0zip.h"
#include "ut0ut.h"
#include "btr0scrub.h"
#include "fsp0fsp.h"
#include "fil0pagecompress.h"
#include "ha_prototypes.h" // IB_LOG_
#include <my_crypt.h>

/** Mutex for keys */
static ib_mutex_t fil_crypt_key_mutex;

static bool fil_crypt_threads_inited = false;

#ifdef UNIV_PFS_MUTEX
static mysql_pfs_key_t fil_crypt_key_mutex_key;
#endif

/** Is encryption enabled/disabled */
UNIV_INTERN ulong srv_encrypt_tables = 0;

/** No of key rotation threads requested */
UNIV_INTERN uint srv_n_fil_crypt_threads = 0;

/** No of key rotation threads started */
UNIV_INTERN uint srv_n_fil_crypt_threads_started = 0;

/** At this age or older a space/page will be rotated */
UNIV_INTERN uint srv_fil_crypt_rotate_key_age;

/** Event to signal FROM the key rotation threads.
Protected with fil_crypt_threads_mutex. */
static os_event_t fil_crypt_event;

/** Event to signal TO the key rotation threads.
Protected with fil_crypt_threads_mutex. */
UNIV_INTERN os_event_t fil_crypt_threads_event;

/** Event for waking up threads throttle.
Protected with fil_crypt_threads_mutex. */
static os_event_t fil_crypt_throttle_sleep_event;

/** Mutex for key rotation threads */
UNIV_INTERN ib_mutex_t fil_crypt_threads_mutex;

#ifdef UNIV_PFS_MUTEX
static mysql_pfs_key_t fil_crypt_threads_mutex_key;
#endif

/** Variable ensuring only 1 thread at time does initial conversion */
static bool fil_crypt_start_converting = false;

/** Variables for throttling */
UNIV_INTERN uint srv_n_fil_crypt_iops = 100;	 // 10ms per iop
static uint srv_alloc_time = 3;		    // allocate iops for 3s at a time
static uint n_fil_crypt_iops_allocated = 0;

/** Variables for scrubbing */
extern uint srv_background_scrub_data_interval;
extern uint srv_background_scrub_data_check_interval;

#define DEBUG_KEYROTATION_THROTTLING 0

/** Statistics variables */
static fil_crypt_stat_t crypt_stat;
static ib_mutex_t crypt_stat_mutex;

#ifdef UNIV_PFS_MUTEX
static mysql_pfs_key_t fil_crypt_stat_mutex_key;

/**
 * key for crypt data mutex
*/
UNIV_INTERN mysql_pfs_key_t fil_crypt_data_mutex_key;
#endif

/** Is background scrubbing enabled, defined on btr0scrub.cc */
extern my_bool srv_background_scrub_data_uncompressed;
extern my_bool srv_background_scrub_data_compressed;

static bool
fil_crypt_needs_rotation(
/*=====================*/
	fil_encryption_t        encrypt_mode,           /*!< in: Encryption
							mode */
	uint			key_version,		/*!< in: Key version */
	uint			latest_key_version,	/*!< in: Latest key version */
	uint			rotate_key_age);	/*!< in: When to rotate */

/*********************************************************************
Init space crypt */
UNIV_INTERN
void
fil_space_crypt_init()
{
	mutex_create(fil_crypt_key_mutex_key,
		     &fil_crypt_key_mutex, SYNC_NO_ORDER_CHECK);

	fil_crypt_throttle_sleep_event = os_event_create();

	mutex_create(fil_crypt_stat_mutex_key,
		     &crypt_stat_mutex, SYNC_NO_ORDER_CHECK);

	memset(&crypt_stat, 0, sizeof(crypt_stat));
}

/*********************************************************************
Cleanup space crypt */
UNIV_INTERN
void
fil_space_crypt_cleanup()
{
	os_event_free(fil_crypt_throttle_sleep_event);
	mutex_free(&fil_crypt_key_mutex);
	mutex_free(&crypt_stat_mutex);
}

/**
Get latest key version from encryption plugin.
@return key version or ENCRYPTION_KEY_VERSION_INVALID */
uint
fil_space_crypt_t::key_get_latest_version(void)
{
	uint key_version = key_found;

	if (is_key_found()) {
		key_version = encryption_key_get_latest_version(key_id);
		srv_stats.n_key_requests.inc();
		key_found = key_version;
	}

	return key_version;
}

/******************************************************************
Get the latest(key-version), waking the encrypt thread, if needed
@param[in]	crypt_data	Crypt data */
static inline
uint
fil_crypt_get_latest_key_version(
	fil_space_crypt_t* crypt_data)
{
	ut_ad(crypt_data != NULL);

	uint key_version = crypt_data->key_get_latest_version();

	if (crypt_data->is_key_found()) {

		if (fil_crypt_needs_rotation(crypt_data->encryption,
				crypt_data->min_key_version,
				key_version,
				srv_fil_crypt_rotate_key_age)) {
			mutex_enter(&fil_crypt_threads_mutex);
			os_event_set(fil_crypt_threads_event);
			mutex_exit(&fil_crypt_threads_mutex);
		}
	}

	return key_version;
}

/******************************************************************
Mutex helper for crypt_data->scheme */
void
crypt_data_scheme_locker(
/*=====================*/
	st_encryption_scheme*	scheme,
	int			exit)
{
	fil_space_crypt_t* crypt_data =
		static_cast<fil_space_crypt_t*>(scheme);

	if (exit) {
		mutex_exit(&crypt_data->mutex);
	} else {
		mutex_enter(&crypt_data->mutex);
	}
}

/******************************************************************
Create a fil_space_crypt_t object
@param[in]	type		CRYPT_SCHEME_UNENCRYPTE or
				CRYPT_SCHEME_1
@param[in]	encrypt_mode	FIL_ENCRYPTION_DEFAULT or
				FIL_ENCRYPTION_ON or
				FIL_ENCRYPTION_OFF
@param[in]	min_key_version key_version or 0
@param[in]	key_id		Used key id
@param[in]	offset		Page 0 offset
@return crypt object */
static
fil_space_crypt_t*
fil_space_create_crypt_data(
	uint			type,
	fil_encryption_t	encrypt_mode,
	uint			min_key_version,
	uint			key_id,
	ulint			offset)
{
	void* buf = mem_zalloc(sizeof(fil_space_crypt_t));
	fil_space_crypt_t* crypt_data = NULL;

	if (buf) {
		crypt_data = new(buf)
			fil_space_crypt_t(
				type,
				min_key_version,
				key_id,
				offset,
				encrypt_mode);
	}

	return crypt_data;
}

/******************************************************************
Create a fil_space_crypt_t object
@param[in]	encrypt_mode	FIL_ENCRYPTION_DEFAULT or
				FIL_ENCRYPTION_ON or
				FIL_ENCRYPTION_OFF

@param[in]	key_id		Encryption key id
@return crypt object */
UNIV_INTERN
fil_space_crypt_t*
fil_space_create_crypt_data(
	fil_encryption_t	encrypt_mode,
	uint			key_id)
{
	return (fil_space_create_crypt_data(0, encrypt_mode, 0, key_id, 0));
}

/******************************************************************
Merge fil_space_crypt_t object
@param[in,out]	dst		Destination cryp data
@param[in]	src		Source crypt data */
UNIV_INTERN
void
fil_space_merge_crypt_data(
	fil_space_crypt_t* dst,
	const fil_space_crypt_t* src)
{
	mutex_enter(&dst->mutex);

	/* validate that they are mergeable */
	ut_a(src->type == CRYPT_SCHEME_UNENCRYPTED ||
	     src->type == CRYPT_SCHEME_1);

	ut_a(dst->type == CRYPT_SCHEME_UNENCRYPTED ||
	     dst->type == CRYPT_SCHEME_1);

	dst->encryption = src->encryption;
	dst->type = src->type;
	dst->min_key_version = src->min_key_version;
	dst->keyserver_requests += src->keyserver_requests;

	mutex_exit(&dst->mutex);
}

/******************************************************************
Read crypt data from a page (0)
@param[in]	space		space_id
@param[in]	page		Page 0
@param[in]	offset		offset to crypt data
@return crypt data from page 0. */
UNIV_INTERN
fil_space_crypt_t*
fil_space_read_crypt_data(
	const ulint	space,
	const byte*	page,
	const ulint	offset)
{
	if (memcmp(page + offset, CRYPT_MAGIC, MAGIC_SZ) != 0) {
		/* Crypt data is not stored. */
		return NULL;
	}

	ulint type = mach_read_from_1(page + offset + MAGIC_SZ + 0);

	if (! (type == CRYPT_SCHEME_UNENCRYPTED ||
	       type == CRYPT_SCHEME_1)) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Found non sensible crypt scheme: " ULINTPF " for space " ULINTPF
			" offset: " ULINTPF " bytes: "
			"[ %.2x %.2x %.2x %.2x %.2x %.2x ].",
			type, space, offset,
			page[offset + 0 + MAGIC_SZ],
			page[offset + 1 + MAGIC_SZ],
			page[offset + 2 + MAGIC_SZ],
			page[offset + 3 + MAGIC_SZ],
			page[offset + 4 + MAGIC_SZ],
			page[offset + 5 + MAGIC_SZ]);
		ut_error;
	}

	fil_space_crypt_t* crypt_data;
	ulint iv_length = mach_read_from_1(page + offset + MAGIC_SZ + 1);

	if (! (iv_length == sizeof(crypt_data->iv))) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Found non sensible iv length: %lu for space %lu "
			" offset: %lu type: %lu bytes: "
			"[ %.2x %.2x %.2x %.2x %.2x %.2x ].",
			iv_length, space, offset, type,
			page[offset + 0 + MAGIC_SZ],
			page[offset + 1 + MAGIC_SZ],
			page[offset + 2 + MAGIC_SZ],
			page[offset + 3 + MAGIC_SZ],
			page[offset + 4 + MAGIC_SZ],
			page[offset + 5 + MAGIC_SZ]);
		ut_error;
	}

	uint min_key_version = mach_read_from_4
		(page + offset + MAGIC_SZ + 2 + iv_length);

	uint key_id = mach_read_from_4
		(page + offset + MAGIC_SZ + 2 + iv_length + 4);

	fil_encryption_t encryption = (fil_encryption_t)mach_read_from_1(
		page + offset + MAGIC_SZ + 2 + iv_length + 8);

	crypt_data = fil_space_create_crypt_data(encryption, key_id);
	/* We need to overwrite these as above function will initialize
	members */
	crypt_data->type = type;
	crypt_data->min_key_version = min_key_version;
	crypt_data->page0_offset = offset;
	memcpy(crypt_data->iv, page + offset + MAGIC_SZ + 2, iv_length);

	return crypt_data;
}

/******************************************************************
Free a crypt data object
@param[in,out] crypt_data	crypt data to be freed */
UNIV_INTERN
void
fil_space_destroy_crypt_data(
	fil_space_crypt_t **crypt_data)
{
	if (crypt_data != NULL && (*crypt_data) != NULL) {
		fil_space_crypt_t* c = *crypt_data;
		c->~fil_space_crypt_t();
		mem_free(c);
		*crypt_data = NULL;
	}
}

/******************************************************************
Write crypt data to a page (0)
@param[in,out]	crypt_data	Crypt data
@param[in]	type		Crypt scheme
@param[in,out]	page		Page 0 where to write
@param[in]	offset		Offset where to write
@param[in,out]	mtr		Minitransaction */
static
void
fil_space_write_crypt_data_low(
	fil_space_crypt_t*	crypt_data,
	const ulint		type,
	byte* 			page,
	const ulint		offset,
	mtr_t*			mtr)
{
	ut_a(offset > 0 && offset < UNIV_PAGE_SIZE);
	ulint space_id = mach_read_from_4(
		page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	const uint len = sizeof(crypt_data->iv);
	const uint min_key_version = crypt_data->min_key_version;
	const uint key_id = crypt_data->key_id;
	const fil_encryption_t encryption = crypt_data->encryption;
	crypt_data->page0_offset = offset;

	/*
	redo log this as bytewise updates to page 0
	followed by an MLOG_FILE_WRITE_CRYPT_DATA
	(that will during recovery update fil_space_t)
	*/
	mlog_write_string(page + offset, CRYPT_MAGIC, MAGIC_SZ, mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 0, type, MLOG_1BYTE, mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 1, len, MLOG_1BYTE, mtr);
	mlog_write_string(page + offset + MAGIC_SZ + 2, crypt_data->iv, len,
			  mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 2 + len, min_key_version,
			 MLOG_4BYTES, mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 2 + len + 4, key_id,
			 MLOG_4BYTES, mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 2 + len + 8, encryption,
		MLOG_1BYTE, mtr);

	byte* log_ptr = mlog_open(mtr, 11 + 17 + len);

	if (log_ptr != NULL) {
		log_ptr = mlog_write_initial_log_record_fast(
			page,
			MLOG_FILE_WRITE_CRYPT_DATA,
			log_ptr, mtr);
		mach_write_to_4(log_ptr, space_id);
		log_ptr += 4;
		mach_write_to_2(log_ptr, offset);
		log_ptr += 2;
		mach_write_to_1(log_ptr, type);
		log_ptr += 1;
		mach_write_to_1(log_ptr, len);
		log_ptr += 1;
		mach_write_to_4(log_ptr, min_key_version);
		log_ptr += 4;
		mach_write_to_4(log_ptr, key_id);
		log_ptr += 4;
		mach_write_to_1(log_ptr, encryption);
		log_ptr += 1;
		mlog_close(mtr, log_ptr);

		mlog_catenate_string(mtr, crypt_data->iv, len);
	}
}

/******************************************************************
Write crypt data to a page (0)
@param[in]	space		Space
@param[in,out]	page		Page 0 where to write
@param[in]	offset		Offset where to write
@param[in,out]	mtr		Minitransaction */
UNIV_INTERN
void
fil_space_write_crypt_data(
	const fil_space_t*	space,
	byte* 			page,
	const ulint		offset,
	mtr_t*			mtr)
{
	/* If no crypt data is stored on memory cache for this space,
	then do not continue writing crypt data to page 0. */
	if (space->crypt_data == NULL) {
		return;
	}

	fil_space_write_crypt_data_low(space->crypt_data,
					space->crypt_data->type,
					page, offset, mtr);
}

/******************************************************************
Parse a MLOG_FILE_WRITE_CRYPT_DATA log entry
@param[in]	ptr		Log entry start
@param[in]	end_ptr		Log entry end
@param[in]	block		buffer block
@return position on log buffer */
UNIV_INTERN
const byte*
fil_parse_write_crypt_data(
	const byte*		ptr,
	const byte*		end_ptr,
	const buf_block_t*	block)
{
	/* check that redo log entry is complete */
	uint entry_size =
		4 + // size of space_id
		2 + // size of offset
		1 + // size of type
		1 + // size of iv-len
		4 +  // size of min_key_version
		4 +  // size of key_id
		1; // fil_encryption_t

	if (end_ptr - ptr < entry_size){
		return NULL;
	}

	ulint space_id = mach_read_from_4(ptr);
	ptr += 4;
	uint offset = mach_read_from_2(ptr);
	ptr += 2;
	uint type = mach_read_from_1(ptr);
	ptr += 1;
	uint len = mach_read_from_1(ptr);
	ptr += 1;

	ut_a(type == CRYPT_SCHEME_UNENCRYPTED ||
	     type == CRYPT_SCHEME_1); // only supported

	ut_a(len == CRYPT_SCHEME_1_IV_LEN); // only supported
	uint min_key_version = mach_read_from_4(ptr);
	ptr += 4;

	uint key_id = mach_read_from_4(ptr);
	ptr += 4;

	fil_encryption_t encryption = (fil_encryption_t)mach_read_from_1(ptr);
	ptr +=1;

	if (end_ptr - ptr < len) {
		return NULL;
	}

	fil_space_crypt_t* crypt_data = fil_space_create_crypt_data(encryption, key_id);
	/* Need to overwrite these as above will initialize fields. */
	crypt_data->page0_offset = offset;
	crypt_data->min_key_version = min_key_version;
	crypt_data->encryption = encryption;
	memcpy(crypt_data->iv, ptr, len);
	ptr += len;

	/* update fil_space memory cache with crypt_data */
	fil_space_t* space = fil_space_acquire_silent(space_id);

	if (space) {
		crypt_data = fil_space_set_crypt_data(space, crypt_data);
		fil_space_release(space);
	}

	return ptr;
}

/******************************************************************
Encrypt a buffer
@param[in,out]		crypt_data	Crypt data
@param[in]		space		space_id
@param[in]		offset		Page offset
@param[in]		lsn		Log sequence number
@param[in]		src_frame	Page to encrypt
@param[in]		zip_size	Compressed size or 0
@param[in,out]		dst_fram	Output buffer
@return encrypted buffer or NULL */
UNIV_INTERN
byte*
fil_encrypt_buf(
	fil_space_crypt_t* crypt_data,
	const ulint	space,
	const ulint	offset,
	const lsn_t	lsn,
	const byte*	src_frame,
	const ulint	zip_size,
	byte*		dst_frame)
{
	ulint page_size = (zip_size) ? zip_size : UNIV_PAGE_SIZE;
	uint key_version = fil_crypt_get_latest_key_version(crypt_data);

	if (key_version == ENCRYPTION_KEY_VERSION_INVALID) {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Unknown key id %u. Can't continue!\n",
			crypt_data->key_id);
		ut_error;
	}

	ulint orig_page_type = mach_read_from_2(src_frame+FIL_PAGE_TYPE);
	ibool page_compressed = (orig_page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
	ulint header_len = FIL_PAGE_DATA;

	if (page_compressed) {
		header_len += (FIL_PAGE_COMPRESSED_SIZE + FIL_PAGE_COMPRESSION_METHOD_SIZE);
	}

	/* FIL page header is not encrypted */
	memcpy(dst_frame, src_frame, header_len);

	/* Store key version */
	mach_write_to_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, key_version);

	/* Calculate the start offset in a page */
	ulint unencrypted_bytes = header_len + FIL_PAGE_DATA_END;
	ulint srclen = page_size - unencrypted_bytes;
	const byte* src = src_frame + header_len;
	byte* dst = dst_frame + header_len;
	uint32 dstlen = 0;

	if (page_compressed) {
		srclen = mach_read_from_2(src_frame + FIL_PAGE_DATA);
	}

	int rc = encryption_scheme_encrypt(src, srclen, dst, &dstlen,
					   crypt_data, key_version,
					   space, offset, lsn);

	if (! ((rc == MY_AES_OK) && ((ulint) dstlen == srclen))) {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Unable to encrypt data-block "
			" src: %p srclen: %ld buf: %p buflen: %d."
			" return-code: %d. Can't continue!\n",
			src, (long)srclen,
			dst, dstlen, rc);
		ut_error;
	}

	/* For compressed tables we do not store the FIL header because
	the whole page is not stored to the disk. In compressed tables only
	the FIL header + compressed (and now encrypted) payload alligned
	to sector boundary is written. */
	if (!page_compressed) {
		/* FIL page trailer is also not encrypted */
		memcpy(dst_frame + page_size - FIL_PAGE_DATA_END,
			src_frame + page_size - FIL_PAGE_DATA_END,
			FIL_PAGE_DATA_END);
	} else {
		/* Clean up rest of buffer */
		memset(dst_frame+header_len+srclen, 0, page_size - (header_len+srclen));
	}

	/* handle post encryption checksum */
	ib_uint32_t checksum = 0;

	checksum = fil_crypt_calculate_checksum(zip_size, dst_frame);

	// store the post-encryption checksum after the key-version
	mach_write_to_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4, checksum);

	ut_ad(fil_space_verify_crypt_checksum(dst_frame, zip_size, NULL, offset));

	srv_stats.pages_encrypted.inc();

	return dst_frame;
}

/******************************************************************
Encrypt a page

@param[in]		space_id	space_id
@param[in]		offset		Page offset
@param[in]		lsn		Log sequence number
@param[in]		src_frame	Page to encrypt
@param[in]		zip_size	Compressed size or 0
@param[in,out]		dst_fram	Output buffer
@return encrypted buffer or NULL */
UNIV_INTERN
byte*
fil_space_encrypt(
	ulint		space_id,
	ulint		offset,
	lsn_t		lsn,
	byte*		src_frame,
	ulint		zip_size,
	byte*		dst_frame)
{
	ulint orig_page_type = mach_read_from_2(src_frame+FIL_PAGE_TYPE);

	if (orig_page_type==FIL_PAGE_TYPE_FSP_HDR
		|| orig_page_type==FIL_PAGE_TYPE_XDES) {
		/* File space header or extent descriptor do not need to be
		encrypted. */
		return src_frame;
	}

	fil_space_t* space = fil_space_acquire(space_id);

	if (!space || !space->crypt_data) {
		return src_frame;
	}

	fil_space_crypt_t* crypt_data = space->crypt_data;
	ut_a(crypt_data != NULL && crypt_data->is_encrypted());

	byte* tmp = fil_encrypt_buf(crypt_data, space->id, offset, lsn, src_frame, zip_size, dst_frame);

#ifdef UNIV_DEBUG
	if (tmp) {
		/* Verify that encrypted buffer is not corrupted */
		byte* tmp_mem = (byte *)malloc(UNIV_PAGE_SIZE);
		dberr_t err = DB_SUCCESS;
		byte* src = src_frame;
		bool page_compressed_encrypted = (mach_read_from_2(tmp+FIL_PAGE_TYPE) == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
		byte* comp_mem = NULL;
		byte* uncomp_mem = NULL;
		ulint size = (zip_size) ? zip_size : UNIV_PAGE_SIZE;

		if (page_compressed_encrypted) {
			comp_mem = (byte *)malloc(UNIV_PAGE_SIZE);
			uncomp_mem = (byte *)malloc(UNIV_PAGE_SIZE);
			memcpy(comp_mem, src_frame, UNIV_PAGE_SIZE);
			fil_decompress_page(uncomp_mem, comp_mem, UNIV_PAGE_SIZE, NULL);
			src = uncomp_mem;
		}

		bool corrupted1 = buf_page_is_corrupted(true, src, zip_size, space);
		bool ok = fil_space_decrypt(crypt_data, tmp_mem, size, tmp, &err);

		/* Need to decompress the page if it was also compressed */
		if (page_compressed_encrypted) {
			memcpy(comp_mem, tmp_mem, UNIV_PAGE_SIZE);
			fil_decompress_page(tmp_mem, comp_mem, UNIV_PAGE_SIZE, NULL);
		}

		bool corrupted = buf_page_is_corrupted(true, tmp_mem, zip_size, space);
		bool different = memcmp(src, tmp_mem, size);

		if (!ok || corrupted || corrupted1 || err != DB_SUCCESS || different) {
			fprintf(stderr, "ok %d corrupted %d corrupted1 %d err %d different %d\n", ok , corrupted, corrupted1, err, different);
			fprintf(stderr, "src_frame\n");
			buf_page_print(src_frame, zip_size, BUF_PAGE_PRINT_NO_CRASH);
			fprintf(stderr, "encrypted_frame\n");
			buf_page_print(tmp, zip_size, BUF_PAGE_PRINT_NO_CRASH);
			fprintf(stderr, "decrypted_frame\n");
			buf_page_print(tmp_mem, zip_size, 0);
		}

		free(tmp_mem);

		if (comp_mem) {
			free(comp_mem);
		}

		if (uncomp_mem) {
			free(uncomp_mem);
		}
	}

#endif /* UNIV_DEBUG */

	fil_space_release(space);

	return tmp;
}

/******************************************************************
Decrypt a page
@param[in]	crypt_data		crypt_data
@param[in]	tmp_frame		Temporary buffer
@param[in]	page_size		Page size
@param[in,out]	src_frame		Page to decrypt
@param[out]	err			DB_SUCCESS or error
@return true if page decrypted, false if not.*/
UNIV_INTERN
bool
fil_space_decrypt(
	fil_space_crypt_t*	crypt_data,
	byte*			tmp_frame,
	const ulint		page_size,
	byte*			src_frame,
	dberr_t*		err)
{
	ulint page_type = mach_read_from_2(src_frame+FIL_PAGE_TYPE);
	uint key_version = mach_read_from_4(src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
	bool page_compressed = (page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
	ulint offset = mach_read_from_4(src_frame + FIL_PAGE_OFFSET);
	ulint space = mach_read_from_4(src_frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	ib_uint64_t lsn = mach_read_from_8(src_frame + FIL_PAGE_LSN);
	*err = DB_SUCCESS;

	if (key_version == ENCRYPTION_KEY_NOT_ENCRYPTED) {
		return false;
	}

	if (crypt_data == NULL) {
		if (!(space == 0 && offset == 0) && key_version != 0) {
			/* FIL_PAGE_FILE_FLUSH_LSN field i.e.
			FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
			should be only defined for the
			first page in a system tablespace
			data file (ibdata*, not *.ibd), if not
			clear it. */
#ifdef UNIV_DEBUG
			ib_logf(IB_LOG_LEVEL_WARN,
				"Page on space %lu offset %lu has key_version %u"
				" when it shoud be undefined.",
				space, offset, key_version);
#endif
			mach_write_to_4(src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0);
		}
		return false;
	}

	ut_a(crypt_data != NULL && crypt_data->is_encrypted());

	/* read space & lsn */
	ulint header_len = FIL_PAGE_DATA;

	if (page_compressed) {
		header_len += (FIL_PAGE_COMPRESSED_SIZE + FIL_PAGE_COMPRESSION_METHOD_SIZE);
	}

	/* Copy FIL page header, it is not encrypted */
	memcpy(tmp_frame, src_frame, header_len);

	/* Calculate the offset where decryption starts */
	const byte* src = src_frame + header_len;
	byte* dst = tmp_frame + header_len;
	uint32 dstlen = 0;
	ulint srclen = page_size - (header_len + FIL_PAGE_DATA_END);

	if (page_compressed) {
		srclen = mach_read_from_2(src_frame + FIL_PAGE_DATA);
	}

	int rc = encryption_scheme_decrypt(src, srclen, dst, &dstlen,
					   crypt_data, key_version,
					   space, offset, lsn);

	if (! ((rc == MY_AES_OK) && ((ulint) dstlen == srclen))) {

		if (rc == -1) {
			*err = DB_DECRYPTION_FAILED;
			return false;
		}

		ib_logf(IB_LOG_LEVEL_FATAL,
			"Unable to decrypt data-block "
			" src: %p srclen: %ld buf: %p buflen: %d."
			" return-code: %d. Can't continue!\n",
			src, (long)srclen,
			dst, dstlen, rc);
		ut_error;
	}

	/* For compressed tables we do not store the FIL header because
	the whole page is not stored to the disk. In compressed tables only
	the FIL header + compressed (and now encrypted) payload alligned
	to sector boundary is written. */
	if (!page_compressed) {
		/* Copy FIL trailer */
		memcpy(tmp_frame + page_size - FIL_PAGE_DATA_END,
		       src_frame + page_size - FIL_PAGE_DATA_END,
		       FIL_PAGE_DATA_END);

		// clear key-version & crypt-checksum from dst
		memset(tmp_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
	}

	srv_stats.pages_decrypted.inc();

	return true; /* page was decrypted */
}

/******************************************************************
Decrypt a page
@param[in]	space_id		Tablespace id
@param[in]	tmp_frame		Temporary buffer used for decrypting
@param[in]	page_size		Page size
@param[in,out]	src_frame		Page to decrypt
@param[out]	decrypted		true if page was decrypted
@return decrypted page, or original not encrypted page if decryption is
not needed.*/
UNIV_INTERN
byte*
fil_space_decrypt(
	ulint		space_id,
	byte*		tmp_frame,
	const ulint	page_size,
	byte*		src_frame,
	bool*		decrypted)
{
	dberr_t err = DB_SUCCESS;
	byte* res = NULL;
	*decrypted = false;

	fil_space_t* space = fil_space_acquire(space_id);

	if (!space) {
		return res;
	}

	bool encrypted = fil_space_decrypt(
				space->crypt_data,
				tmp_frame,
				page_size,
				src_frame,
				&err);

	if (err == DB_SUCCESS) {
		if (encrypted) {
			*decrypted = true;
			/* Copy the decrypted page back to page buffer, not
			really any other options. */
			memcpy(src_frame, tmp_frame, page_size);
		}

		res = src_frame;
	}

	fil_space_release(space);
	return res;
}

/******************************************************************
Calculate post encryption checksum
@param[in]	zip_size	zip_size or 0
@param[in]	dst_frame	Block where checksum is calculated
@return page checksum or BUF_NO_CHECKSUM_MAGIC
not needed. */
UNIV_INTERN
ulint
fil_crypt_calculate_checksum(
	const ulint	zip_size,
	const byte*	dst_frame)
{
	ib_uint32_t checksum = 0;
	srv_checksum_algorithm_t algorithm =
			static_cast<srv_checksum_algorithm_t>(srv_checksum_algorithm);

	if (zip_size == 0) {
		switch (algorithm) {
		case SRV_CHECKSUM_ALGORITHM_CRC32:
		case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
			checksum = buf_calc_page_crc32(dst_frame);
			break;
		case SRV_CHECKSUM_ALGORITHM_INNODB:
		case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
			checksum = (ib_uint32_t) buf_calc_page_new_checksum(
				dst_frame);
			break;
		case SRV_CHECKSUM_ALGORITHM_NONE:
		case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
			checksum = BUF_NO_CHECKSUM_MAGIC;
			break;
			/* no default so the compiler will emit a warning
			* if new enum is added and not handled here */
		}
	} else {
		checksum = page_zip_calc_checksum(dst_frame, zip_size,
				                          algorithm);
	}

	return checksum;
}

/*********************************************************************
Verify that post encryption checksum match calculated checksum.
This function should be called only if tablespace contains crypt_data
metadata (this is strong indication that tablespace is encrypted).
Function also verifies that traditional checksum does not match
calculated checksum as if it does page could be valid unencrypted,
encrypted, or corrupted.

@param[in]	page		Page to verify
@param[in]	zip_size	zip size
@param[in]	space		Tablespace
@param[in]	pageno		Page no
@return true if page is encrypted AND OK, false otherwise */
UNIV_INTERN
bool
fil_space_verify_crypt_checksum(
	byte* 			page,
	const ulint		zip_size,
	const fil_space_t*	space,
	const ulint		pageno)
{
	uint key_version = mach_read_from_4(page+ FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

	/* If page is not encrypted, return false */
	if (key_version == 0) {
		return false;
	}

	srv_checksum_algorithm_t algorithm =
			static_cast<srv_checksum_algorithm_t>(srv_checksum_algorithm);
	/* If no checksum is used, can't continue checking. */
	if (algorithm == SRV_CHECKSUM_ALGORITHM_NONE) {
		return(true);
	}

	/* Read stored post encryption checksum. */
	ib_uint32_t checksum = mach_read_from_4(
		page + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4);

	/* Declare empty pages non-corrupted */
	if (checksum == 0
	    && *reinterpret_cast<const ib_uint64_t*>(page + FIL_PAGE_LSN) == 0
	    && buf_page_is_zeroes(page, zip_size)) {
		return(true);
	}

	/* Compressed and encrypted pages do not have checksum. Assume not
	corrupted. Page verification happens after decompression in
	buf_page_io_complete() using buf_page_is_corrupted(). */
	if (mach_read_from_2(page+FIL_PAGE_TYPE) == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		return (true);
	}

	/* Compressed pages use different checksum method. We first store
	the post encryption checksum on checksum location and after function
	restore the original. */
	if (zip_size) {
		ib_uint32_t old = static_cast<ib_uint32_t>(mach_read_from_4(
				page + FIL_PAGE_SPACE_OR_CHKSUM));

		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, checksum);

		bool valid = page_zip_verify_checksum(page, zip_size);

		mach_write_to_4(page + FIL_PAGE_SPACE_OR_CHKSUM, old);

		return (valid);
	}

	/* If stored checksum matches one of the calculated checksums
	page is not corrupted. */

	ib_uint32_t cchecksum1 = buf_calc_page_crc32(page);
	ib_uint32_t cchecksum2 = (ib_uint32_t) buf_calc_page_new_checksum(
				page);
	bool encrypted = (checksum == cchecksum1 || checksum == cchecksum2
		|| checksum == BUF_NO_CHECKSUM_MAGIC);

	/* MySQL 5.6 and MariaDB 10.0 and 10.1 will write an LSN to the
	first page of each system tablespace file at
	FIL_PAGE_FILE_FLUSH_LSN offset. On other pages and in other files,
	the field might have been uninitialized until MySQL 5.5. In MySQL 5.7
	(and MariaDB Server 10.2.2) WL#7990 stopped writing the field for other
	than page 0 of the system tablespace.

	Starting from MariaDB 10.1 the field has been repurposed for
	encryption key_version.

	Starting with MySQL 5.7 (and MariaDB Server 10.2), the
	field has been repurposed for SPATIAL INDEX pages for
	FIL_RTREE_SPLIT_SEQ_NUM.

	Note that FIL_PAGE_FILE_FLUSH_LSN is not included in the InnoDB page
	checksum.

	Thus, FIL_PAGE_FILE_FLUSH_LSN could contain any value. While the
	field would usually be 0 for pages that are not encrypted, we cannot
	assume that a nonzero value means that the page is encrypted.
	Therefore we must validate the page both as encrypted and unencrypted
	when FIL_PAGE_FILE_FLUSH_LSN does not contain 0.

	*/

	ulint checksum1 = mach_read_from_4(
		page + FIL_PAGE_SPACE_OR_CHKSUM);

	ulint checksum2 = mach_read_from_4(
		page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM);


	bool valid = (buf_page_is_checksum_valid_crc32(page,checksum1,checksum2)
		|| buf_page_is_checksum_valid_none(page,checksum1,checksum2)
		|| buf_page_is_checksum_valid_innodb(page,checksum1, checksum2));

	if (encrypted && valid) {
		/* If page is encrypted and traditional checksums match,
		page could be still encrypted, or not encrypted and valid or
		corrupted. */
		ib_logf(IB_LOG_LEVEL_ERROR,
			" Page %lu in space %s (%lu) maybe corrupted."
			" Post encryption checksum %u stored [%lu:%lu] key_version %u",
			pageno,
			space ? space->name : "N/A",
			mach_read_from_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID),
			checksum, checksum1, checksum2, key_version);
		encrypted = false;
	}

	return(encrypted);
}

/***********************************************************************/

/** A copy of global key state */
struct key_state_t {
	key_state_t() : key_id(0), key_version(0),
			rotate_key_age(srv_fil_crypt_rotate_key_age) {}
	bool operator==(const key_state_t& other) const {
		return key_version == other.key_version &&
			rotate_key_age == other.rotate_key_age;
	}
	uint key_id;
	uint key_version;
	uint rotate_key_age;
};

/***********************************************************************
Copy global key state
@param[in,out]	new_state	key state
@param[in]	crypt_data	crypt data */
static void
fil_crypt_get_key_state(
	key_state_t*			new_state,
	fil_space_crypt_t*		crypt_data)
{
	if (srv_encrypt_tables) {
		new_state->key_version = crypt_data->key_get_latest_version();
		new_state->rotate_key_age = srv_fil_crypt_rotate_key_age;

		ut_a(new_state->key_version != ENCRYPTION_KEY_NOT_ENCRYPTED);
	} else {
		new_state->key_version = 0;
		new_state->rotate_key_age = 0;
	}
}

/***********************************************************************
Check if a key needs rotation given a key_state
@param[in]	encrypt_mode		Encryption mode
@param[in]	key_version		Current key version
@param[in]	latest_key_version	Latest key version
@param[in]	rotate_key_age		when to rotate
@return true if key needs rotation, false if not */
static bool
fil_crypt_needs_rotation(
	const fil_encryption_t  encrypt_mode,
	const uint		key_version,
	const uint		latest_key_version,
	const uint		rotate_key_age)
{
	if (key_version == ENCRYPTION_KEY_VERSION_INVALID) {
		return false;
	}

	if (key_version == 0 && latest_key_version != 0) {
		/* this is rotation unencrypted => encrypted
		* ignore rotate_key_age */
		return true;
	}

	if (latest_key_version == 0 && key_version != 0) {
		if (encrypt_mode == FIL_ENCRYPTION_DEFAULT) {
			/* this is rotation encrypted => unencrypted */
			return true;
		}
		return false;
	}

	/* this is rotation encrypted => encrypted,
	* only reencrypt if key is sufficiently old */
	if (key_version + rotate_key_age < latest_key_version) {
		return true;
	}

	return false;
}

/******************************************************************
Set crypt data for a tablespace
@param[in,out]		space		Tablespace
@param[in]		crypt_data	Crypt data to be set
@return crypt_data in tablespace */
UNIV_INTERN
fil_space_crypt_t*
fil_space_set_crypt_data(
	fil_space_t*		space,
	fil_space_crypt_t*	crypt_data)
{
	fil_space_crypt_t* free_crypt_data = NULL;
	fil_space_crypt_t* ret_crypt_data = NULL;

	/* Provided space is protected using fil_space_acquire()
	from concurrent operations. */
	if (space->crypt_data != NULL) {
		/* There is already crypt data present,
		merge new crypt_data */
		fil_space_merge_crypt_data(space->crypt_data,
						   crypt_data);
		ret_crypt_data = space->crypt_data;
		free_crypt_data = crypt_data;
	} else {
		space->crypt_data = crypt_data;
		ret_crypt_data = space->crypt_data;
	}

	if (free_crypt_data != NULL) {
		/* there was already crypt data present and the new crypt
		* data provided as argument to this function has been merged
		* into that => free new crypt data
		*/
		fil_space_destroy_crypt_data(&free_crypt_data);
	}

	return ret_crypt_data;
}

/***********************************************************************
Start encrypting a space
@param[in,out]		space		Tablespace
@return true if a recheck is needed */
static
bool
fil_crypt_start_encrypting_space(
	fil_space_t*	space)
{
	bool recheck = false;
	mutex_enter(&fil_crypt_threads_mutex);

	/* Here as page 0 might not yet being read, use
	function that can read it. */
	fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);

	/* If space is not encrypted and encryption is not enabled, then
	do not continue encrypting the space. */
	if (!crypt_data && !srv_encrypt_tables) {
		mutex_exit(&fil_crypt_threads_mutex);
		return false;
	}

	if (crypt_data != NULL || fil_crypt_start_converting) {
		/* someone beat us to it */
		if (fil_crypt_start_converting) {
			recheck = true;
		}

		mutex_exit(&fil_crypt_threads_mutex);
		return recheck;
	}

	/* NOTE: we need to write and flush page 0 before publishing
	* the crypt data. This so that after restart there is no
	* risk of finding encrypted pages without having
	* crypt data in page 0 */

	/* 1 - create crypt data */
	crypt_data = fil_space_create_crypt_data(FIL_ENCRYPTION_DEFAULT, FIL_DEFAULT_ENCRYPTION_KEY);

	if (crypt_data == NULL) {
		mutex_exit(&fil_crypt_threads_mutex);
		return false;
	}

	crypt_data->type = CRYPT_SCHEME_UNENCRYPTED;
	crypt_data->min_key_version = 0; // all pages are unencrypted
	crypt_data->rotate_state.start_time = time(0);
	crypt_data->rotate_state.starting = true;
	crypt_data->rotate_state.active_threads = 1;

	mutex_enter(&crypt_data->mutex);
	crypt_data = fil_space_set_crypt_data(space, crypt_data);
	mutex_exit(&crypt_data->mutex);

	fil_crypt_start_converting = true;
	mutex_exit(&fil_crypt_threads_mutex);

	do
	{
		mtr_t mtr;
		mtr_start(&mtr);

		/* 2 - get page 0 */
		ulint offset = 0;
		ulint zip_size = fsp_flags_get_zip_size(space->flags);
		buf_block_t* block = buf_page_get_gen(space->id, zip_size, offset,
						      RW_X_LATCH,
						      NULL,
						      BUF_GET,
						      __FILE__, __LINE__,
						      &mtr);

		/* 3 - compute location to store crypt data */
		byte* frame = buf_block_get_frame(block);

		crypt_data->page0_offset =
			fsp_header_get_crypt_offset(zip_size);

		/* 4 - write crypt data to page 0 */
		fil_space_write_crypt_data_low(crypt_data,
					       CRYPT_SCHEME_1,
					       frame,
					       crypt_data->page0_offset,
					       &mtr);

		mtr_commit(&mtr);

		/* record lsn of update */
		lsn_t end_lsn = mtr.end_lsn;

		/* 4 - sync tablespace before publishing crypt data */

		bool success = false;
		ulint sum_pages = 0;

		do {
			ulint n_pages = 0;
			success = buf_flush_list(ULINT_MAX, end_lsn, &n_pages);
			buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
			sum_pages += n_pages;
		} while (!success);

		/* 5 - publish crypt data */
		mutex_enter(&fil_crypt_threads_mutex);
		mutex_enter(&crypt_data->mutex);
		crypt_data->type = CRYPT_SCHEME_1;
		ut_a(crypt_data->rotate_state.active_threads == 1);
		crypt_data->rotate_state.active_threads = 0;
		crypt_data->rotate_state.starting = false;

		fil_crypt_start_converting = false;
		mutex_exit(&crypt_data->mutex);
		mutex_exit(&fil_crypt_threads_mutex);

		return recheck;
	} while (0);

	mutex_enter(&crypt_data->mutex);
	ut_a(crypt_data->rotate_state.active_threads == 1);
	crypt_data->rotate_state.active_threads = 0;
	mutex_exit(&crypt_data->mutex);

	mutex_enter(&fil_crypt_threads_mutex);
	fil_crypt_start_converting = false;
	mutex_exit(&fil_crypt_threads_mutex);

	return recheck;
}

/** State of a rotation thread */
struct rotate_thread_t {
	explicit rotate_thread_t(uint no) {
		memset(this, 0, sizeof(* this));
		thread_no = no;
		first = true;
		estimated_max_iops = 20;
	}

	uint thread_no;
	bool first;		    /*!< is position before first space */
	fil_space_t* space;	    /*!< current space or NULL */
	ulint offset;		    /*!< current offset */
	ulint batch;		    /*!< #pages to rotate */
	uint  min_key_version_found;/*!< min key version found but not rotated */
	lsn_t end_lsn;		    /*!< max lsn when rotating this space */

	uint estimated_max_iops;   /*!< estimation of max iops */
	uint allocated_iops;	   /*!< allocated iops */
	uint cnt_waited;	   /*!< #times waited during this slot */
	uint sum_waited_us;	   /*!< wait time during this slot */

	fil_crypt_stat_t crypt_stat; // statistics

	btr_scrub_t scrub_data;      /* thread local data used by btr_scrub-functions
				     * when iterating pages of tablespace */

	/** @return whether this thread should terminate */
	bool should_shutdown() const {
		switch (srv_shutdown_state) {
		case SRV_SHUTDOWN_NONE:
		case SRV_SHUTDOWN_CLEANUP:
			return thread_no >= srv_n_fil_crypt_threads;
		case SRV_SHUTDOWN_FLUSH_PHASE:
			return true;
		case SRV_SHUTDOWN_LAST_PHASE:
		case SRV_SHUTDOWN_EXIT_THREADS:
			break;
		}
		ut_ad(0);
		return true;
	}
};

/***********************************************************************
Check if space needs rotation given a key_state
@param[in,out]		state		Key rotation state
@param[in,out]		key_state	Key state
@param[in,out]		recheck		needs recheck ?
@return true if space needs key rotation */
static
bool
fil_crypt_space_needs_rotation(
	rotate_thread_t*	state,
	key_state_t*		key_state,
	bool*			recheck)
{
	fil_space_t* space = state->space;

	/* Make sure that tablespace is normal tablespace */
	if (space->purpose != FIL_TABLESPACE) {
		return false;
	}

	/* Need to read page 0 if not yet read */
	fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);

	if (crypt_data == NULL) {
		/**
		* space has no crypt data
		*   start encrypting it...
		*/
		*recheck = fil_crypt_start_encrypting_space(space);
		crypt_data = space->crypt_data;

		if (crypt_data == NULL) {
			return false;
		}

		crypt_data->key_get_latest_version();
	}

	/* If used key_id is not found from encryption plugin we can't
	continue to rotate the tablespace */
	if (!crypt_data->is_key_found()) {
		return false;
	}

	mutex_enter(&crypt_data->mutex);

	do {
		/* prevent threads from starting to rotate space */
		if (crypt_data->rotate_state.starting) {
			/* recheck this space later */
			*recheck = true;
			break;
		}

		/* prevent threads from starting to rotate space */
		if (space->stop_new_ops) {
			break;
		}

		if (crypt_data->rotate_state.flushing) {
			break;
		}

		/* No need to rotate space if encryption is disabled */
		if (crypt_data->not_encrypted()) {
			break;
		}

		if (crypt_data->key_id != key_state->key_id) {
			key_state->key_id= crypt_data->key_id;
			fil_crypt_get_key_state(key_state, crypt_data);
		}

		bool need_key_rotation = fil_crypt_needs_rotation(
			crypt_data->encryption,
			crypt_data->min_key_version,
			key_state->key_version, key_state->rotate_key_age);

		crypt_data->rotate_state.scrubbing.is_active =
			btr_scrub_start_space(space->id, &state->scrub_data);

		time_t diff = time(0) - crypt_data->rotate_state.scrubbing.
			last_scrub_completed;

		bool need_scrubbing =
			(srv_background_scrub_data_uncompressed ||
			 srv_background_scrub_data_compressed) &&
			crypt_data->rotate_state.scrubbing.is_active
			&& diff >= srv_background_scrub_data_interval;

		if (need_key_rotation == false && need_scrubbing == false) {
			break;
		}

		mutex_exit(&crypt_data->mutex);

		return true;
	} while (0);

	mutex_exit(&crypt_data->mutex);


	return false;
}

/***********************************************************************
Update global statistics with thread statistics
@param[in,out]	state		key rotation statistics */
static void
fil_crypt_update_total_stat(
	rotate_thread_t *state)
{
	mutex_enter(&crypt_stat_mutex);
	crypt_stat.pages_read_from_cache +=
		state->crypt_stat.pages_read_from_cache;
	crypt_stat.pages_read_from_disk +=
		state->crypt_stat.pages_read_from_disk;
	crypt_stat.pages_modified += state->crypt_stat.pages_modified;
	crypt_stat.pages_flushed += state->crypt_stat.pages_flushed;
	// remote old estimate
	crypt_stat.estimated_iops -= state->crypt_stat.estimated_iops;
	// add new estimate
	crypt_stat.estimated_iops += state->estimated_max_iops;
	mutex_exit(&crypt_stat_mutex);

	// make new estimate "current" estimate
	memset(&state->crypt_stat, 0, sizeof(state->crypt_stat));
	// record our old (current) estimate
	state->crypt_stat.estimated_iops = state->estimated_max_iops;
}

/***********************************************************************
Allocate iops to thread from global setting,
used before starting to rotate a space.
@param[in,out]		state		Rotation state
@return true if allocation succeeded, false if failed */
static
bool
fil_crypt_alloc_iops(
	rotate_thread_t *state)
{
	ut_ad(state->allocated_iops == 0);

	uint max_iops = state->estimated_max_iops;
	mutex_enter(&fil_crypt_threads_mutex);

	if (n_fil_crypt_iops_allocated >= srv_n_fil_crypt_iops) {
		/* this can happen when user decreases srv_fil_crypt_iops */
		mutex_exit(&fil_crypt_threads_mutex);
		return false;
	}

	uint alloc = srv_n_fil_crypt_iops - n_fil_crypt_iops_allocated;

	if (alloc > max_iops) {
		alloc = max_iops;
	}

	n_fil_crypt_iops_allocated += alloc;
	mutex_exit(&fil_crypt_threads_mutex);

	state->allocated_iops = alloc;

	return alloc > 0;
}

/***********************************************************************
Reallocate iops to thread,
used when inside a space
@param[in,out]		state		Rotation state */
static
void
fil_crypt_realloc_iops(
	rotate_thread_t *state)
{
	ut_a(state->allocated_iops > 0);

	if (10 * state->cnt_waited > state->batch) {
		/* if we waited more than 10% re-estimate max_iops */
		uint avg_wait_time_us =
			state->sum_waited_us / state->cnt_waited;

#if DEBUG_KEYROTATION_THROTTLING
		ib_logf(IB_LOG_LEVEL_INFO,
			"thr_no: %u - update estimated_max_iops from %u to %u.",
			state->thread_no,
			state->estimated_max_iops,
			1000000 / avg_wait_time_us);
#endif
		if (avg_wait_time_us == 0) {
			avg_wait_time_us = 1; // prevent division by zero
		}

		state->estimated_max_iops = 1000000 / avg_wait_time_us;
		state->cnt_waited = 0;
		state->sum_waited_us = 0;
	} else {
#if DEBUG_KEYROTATION_THROTTLING
		ib_logf(IB_LOG_LEVEL_INFO,
			"thr_no: %u only waited %lu%% skip re-estimate.",
			state->thread_no,
			(100 * state->cnt_waited) / state->batch);
#endif
	}

	if (state->estimated_max_iops <= state->allocated_iops) {
		/* return extra iops */
		uint extra = state->allocated_iops - state->estimated_max_iops;

		if (extra > 0) {
			mutex_enter(&fil_crypt_threads_mutex);
			if (n_fil_crypt_iops_allocated < extra) {
				/* unknown bug!
				* crash in debug
				* keep n_fil_crypt_iops_allocated unchanged
				* in release */
				ut_ad(0);
				extra = 0;
			}
			n_fil_crypt_iops_allocated -= extra;
			state->allocated_iops -= extra;

			if (state->allocated_iops == 0) {
				/* no matter how slow io system seems to be
				* never decrease allocated_iops to 0... */
				state->allocated_iops ++;
				n_fil_crypt_iops_allocated ++;
			}

			os_event_set(fil_crypt_threads_event);
			mutex_exit(&fil_crypt_threads_mutex);
		}
	} else {
		/* see if there are more to get */
		mutex_enter(&fil_crypt_threads_mutex);
		if (n_fil_crypt_iops_allocated < srv_n_fil_crypt_iops) {
			/* there are extra iops free */
			uint extra = srv_n_fil_crypt_iops -
				n_fil_crypt_iops_allocated;
			if (state->allocated_iops + extra >
			    state->estimated_max_iops) {
				/* but don't alloc more than our max */
				extra = state->estimated_max_iops -
					state->allocated_iops;
			}
			n_fil_crypt_iops_allocated += extra;
			state->allocated_iops += extra;
#if DEBUG_KEYROTATION_THROTTLING
			ib_logf(IB_LOG_LEVEL_INFO,
				"thr_no: %u increased iops from %u to %u.",
				state->thread_no,
				state->allocated_iops - extra,
				state->allocated_iops);
#endif
		}
		mutex_exit(&fil_crypt_threads_mutex);
	}

	fil_crypt_update_total_stat(state);
}

/***********************************************************************
Return allocated iops to global
@param[in,out]		state		Rotation state */
static
void
fil_crypt_return_iops(
	rotate_thread_t *state)
{
	if (state->allocated_iops > 0) {
		uint iops = state->allocated_iops;
		mutex_enter(&fil_crypt_threads_mutex);
		if (n_fil_crypt_iops_allocated < iops) {
			/* unknown bug!
			* crash in debug
			* keep n_fil_crypt_iops_allocated unchanged
			* in release */
			ut_ad(0);
			iops = 0;
		}

		n_fil_crypt_iops_allocated -= iops;
		state->allocated_iops = 0;
		os_event_set(fil_crypt_threads_event);
		mutex_exit(&fil_crypt_threads_mutex);
	}

	fil_crypt_update_total_stat(state);
}

/***********************************************************************
Search for a space needing rotation
@param[in,out]		key_state		Key state
@param[in,out]		state			Rotation state
@param[in,out]		recheck			recheck ? */
static
bool
fil_crypt_find_space_to_rotate(
	key_state_t*		key_state,
	rotate_thread_t*	state,
	bool*			recheck)
{
	/* we need iops to start rotating */
	while (!state->should_shutdown() && !fil_crypt_alloc_iops(state)) {
		mutex_enter(&fil_crypt_threads_mutex);
		os_event_reset(fil_crypt_threads_event);
		mutex_exit(&fil_crypt_threads_mutex);
		os_event_wait_time(fil_crypt_threads_event, 1000000);
	}

	if (state->should_shutdown()) {
		if (state->space) {
			fil_space_release(state->space);
			state->space = NULL;
		}
		return false;
	}

	if (state->first) {
		state->first = false;
		state->space = NULL;
	}

	/* If key rotation is enabled (default) we iterate all tablespaces.
	If key rotation is not enabled we iterate only the tablespaces
	added to keyrotation list. */
	if (srv_fil_crypt_rotate_key_age) {
		state->space = fil_space_next(state->space);
	} else {
		state->space = fil_space_keyrotate_next(state->space);
	}

	while (!state->should_shutdown() && state->space) {
		if (fil_crypt_space_needs_rotation(state, key_state, recheck)) {
			ut_ad(key_state->key_id);
			/* init state->min_key_version_found before
			* starting on a space */
			state->min_key_version_found = key_state->key_version;
			return true;
		}

		if (srv_fil_crypt_rotate_key_age) {
			state->space = fil_space_next(state->space);
		} else {
			state->space = fil_space_keyrotate_next(state->space);
		}
	}

	/* if we didn't find any space return iops */
	fil_crypt_return_iops(state);

	return false;

}

/***********************************************************************
Start rotating a space
@param[in]	key_state		Key state
@param[in,out]	state			Rotation state */
static
void
fil_crypt_start_rotate_space(
	const key_state_t*	key_state,
	rotate_thread_t*	state)
{
	fil_space_crypt_t *crypt_data = state->space->crypt_data;

	ut_ad(crypt_data);
	mutex_enter(&crypt_data->mutex);
	ut_ad(key_state->key_id == crypt_data->key_id);

	if (crypt_data->rotate_state.active_threads == 0) {
		/* only first thread needs to init */
		crypt_data->rotate_state.next_offset = 1; // skip page 0
		/* no need to rotate beyond current max
		* if space extends, it will be encrypted with newer version */
		crypt_data->rotate_state.max_offset = state->space->size;
		crypt_data->rotate_state.end_lsn = 0;
		crypt_data->rotate_state.min_key_version_found =
			key_state->key_version;

		crypt_data->rotate_state.start_time = time(0);

		if (crypt_data->type == CRYPT_SCHEME_UNENCRYPTED &&
			crypt_data->is_encrypted() &&
			key_state->key_version != 0) {
			/* this is rotation unencrypted => encrypted */
			crypt_data->type = CRYPT_SCHEME_1;
		}
	}

	/* count active threads in space */
	crypt_data->rotate_state.active_threads++;

	/* Initialize thread local state */
	state->end_lsn = crypt_data->rotate_state.end_lsn;
	state->min_key_version_found =
		crypt_data->rotate_state.min_key_version_found;

	mutex_exit(&crypt_data->mutex);
}

/***********************************************************************
Search for batch of pages needing rotation
@param[in]	key_state		Key state
@param[in,out]	state			Rotation state
@return true if page needing key rotation found, false if not found */
static
bool
fil_crypt_find_page_to_rotate(
	const key_state_t*	key_state,
	rotate_thread_t*	state)
{
	ulint batch = srv_alloc_time * state->allocated_iops;
	fil_space_t* space = state->space;

	/* If space is marked to be dropped stop rotation. */
	if (!space || space->stop_new_ops) {
		return false;
	}

	fil_space_crypt_t *crypt_data = space->crypt_data;

	/* Space might already be dropped */
	if (crypt_data) {
		mutex_enter(&crypt_data->mutex);
		ut_ad(key_state->key_id == crypt_data->key_id);

		if (!state->space->stop_new_ops &&
			crypt_data->rotate_state.next_offset <
			crypt_data->rotate_state.max_offset) {

			state->offset = crypt_data->rotate_state.next_offset;
			ulint remaining = crypt_data->rotate_state.max_offset -
				crypt_data->rotate_state.next_offset;

			if (batch <= remaining) {
				state->batch = batch;
			} else {
				state->batch = remaining;
			}

			crypt_data->rotate_state.next_offset += batch;
			mutex_exit(&crypt_data->mutex);
			return true;
		}

		mutex_exit(&crypt_data->mutex);
	}

	return false;
}

/***********************************************************************
Check if a page is uninitialized (doesn't need to be rotated)
@param[in]	frame		Page to check
@param[in]	zip_size	zip_size or 0
@return true if page is uninitialized, false if not. */
static inline
bool
fil_crypt_is_page_uninitialized(
	const byte	*frame,
	uint		zip_size)
{
	return (buf_page_is_zeroes(frame, zip_size));
}

#define fil_crypt_get_page_throttle(state,space,zip_size,offset,mtr,sleeptime_ms) \
	fil_crypt_get_page_throttle_func(state, space, zip_size, offset, mtr, \
					 sleeptime_ms, __FILE__, __LINE__)

/***********************************************************************
Get a page and compute sleep time
@param[in,out]		state		Rotation state
@param[in]		space		Tablespace
@param[in]		zip_size	compressed size or 0
@param[in]		offset		Page offset
@param[in,out]		mtr		Minitransaction
@param[out]		sleeptime_ms	Sleep time
@param[in]		file		File where called
@param[in]		line		Line where called
@return page or NULL*/
static
buf_block_t*
fil_crypt_get_page_throttle_func(
	rotate_thread_t*	state,
	const fil_space_t*	space,
	const uint 		zip_size,
	const ulint 		offset,
	mtr_t*			mtr,
	ulint*			sleeptime_ms,
	const char*		file,
	const ulint 		line)
{
	buf_block_t* block = buf_page_try_get_func(space->id, offset, RW_X_LATCH,
						   true,
						   file, line, mtr);
	if (block != NULL) {
		/* page was in buffer pool */
		state->crypt_stat.pages_read_from_cache++;
		return block;
	}

	/* Before reading from tablespace we need to make sure that
	tablespace exists and is not is just being dropped. */
	if (space->stop_new_ops) {
		return NULL;
	}

	state->crypt_stat.pages_read_from_disk++;

	ullint start = ut_time_us(NULL);
	block = buf_page_get_gen(space->id, zip_size, offset,
				 RW_X_LATCH,
				 NULL, BUF_GET_POSSIBLY_FREED,
				 file, line, mtr);
	ullint end = ut_time_us(NULL);

	if (end < start) {
		end = start; // safety...
	}

	state->cnt_waited++;
	state->sum_waited_us += (end - start);

	/* average page load */
	ulint add_sleeptime_ms = 0;
	ulint avg_wait_time_us = state->sum_waited_us / state->cnt_waited;
	ulint alloc_wait_us = 1000000 / state->allocated_iops;

	if (avg_wait_time_us < alloc_wait_us) {
		/* we reading faster than we allocated */
		add_sleeptime_ms = (alloc_wait_us - avg_wait_time_us) / 1000;
	} else {
		/* if page load time is longer than we want, skip sleeping */
	}

	*sleeptime_ms += add_sleeptime_ms;

	return block;
}


/***********************************************************************
Get block and allocation status

note: innodb locks fil_space_latch and then block when allocating page
but locks block and then fil_space_latch when freeing page.

@param[in,out]		state		Rotation state
@param[in]		space		Tablespace
@param[in]		zip_size	Compressed size or 0
@param[in]		offset		Page offset
@param[in,out]		mtr		Minitransaction
@param[out]		allocation_status Allocation status
@param[out]		sleeptime_ms	Sleep time
@return block or NULL
*/
static
buf_block_t*
btr_scrub_get_block_and_allocation_status(
	rotate_thread_t*	state,
	const fil_space_t*	space,
	const uint 		zip_size,
	const ulint 		offset,
	mtr_t*			mtr,
	btr_scrub_page_allocation_status_t *allocation_status,
	ulint*			sleeptime_ms)
{
	mtr_t local_mtr;
	buf_block_t *block = NULL;

	mtr_start(&local_mtr);

	*allocation_status = fsp_page_is_free(space->id, offset, &local_mtr) ?
		BTR_SCRUB_PAGE_FREE :
		BTR_SCRUB_PAGE_ALLOCATED;

	if (*allocation_status == BTR_SCRUB_PAGE_FREE) {
		/* this is easy case, we lock fil_space_latch first and
		then block */
		block = fil_crypt_get_page_throttle(state,
						    space, zip_size,
						    offset, mtr,
						    sleeptime_ms);
		mtr_commit(&local_mtr);
	} else {
		/* page is allocated according to xdes */

		/* release fil_space_latch *before* fetching block */
		mtr_commit(&local_mtr);

		/* NOTE: when we have locked dict_index_get_lock(),
		* it's safe to release fil_space_latch and then fetch block
		* as dict_index_get_lock() is needed to make tree modifications
		* such as free-ing a page
		*/

		block = fil_crypt_get_page_throttle(state,
						    space, zip_size,
						    offset, mtr,
						    sleeptime_ms);
	}

	return block;
}


/***********************************************************************
Rotate one page
@param[in,out]		key_state		Key state
@param[in,out]		state			Rotation state */
static
void
fil_crypt_rotate_page(
	const key_state_t*	key_state,
	rotate_thread_t*	state)
{
	fil_space_t*space = state->space;
	ulint space_id = space->id;
	ulint offset = state->offset;
	const uint zip_size = fsp_flags_get_zip_size(space->flags);
	ulint sleeptime_ms = 0;
	fil_space_crypt_t *crypt_data = space->crypt_data;

	/* In fil_crypt_thread where key rotation is done we have
	acquired space and checked that this space is not yet
	marked to be dropped. Similarly, in fil_crypt_find_page_to_rotate().
	Check here also to give DROP TABLE or similar a change. */
	if (space->stop_new_ops) {
		return;
	}

	if (space == TRX_SYS_SPACE && offset == TRX_SYS_PAGE_NO) {
		/* don't encrypt this as it contains address to dblwr buffer */
		return;
	}

	mtr_t mtr;
	mtr_start(&mtr);
	buf_block_t* block = fil_crypt_get_page_throttle(state,
							 space, zip_size,
							 offset, &mtr,
							 &sleeptime_ms);

	if (block) {

		bool modified = false;
		int needs_scrubbing = BTR_SCRUB_SKIP_PAGE;
		lsn_t block_lsn = block->page.newest_modification;
		uint kv =  block->page.key_version;

		/* check if tablespace is closing after reading page */
		if (space->stop_new_ops) {
			byte* frame = buf_block_get_frame(block);

			if (kv == 0 &&
				fil_crypt_is_page_uninitialized(frame, zip_size)) {
				;
			} else if (fil_crypt_needs_rotation(
					crypt_data->encryption,
					kv, key_state->key_version,
					key_state->rotate_key_age)) {

				/* page can be "fresh" i.e never written in case
				* kv == 0 or it should have a key version at least
				* as big as the space minimum key version*/
				ut_a(kv == 0 || kv >= crypt_data->min_key_version);

				modified = true;

				/* force rotation by dummy updating page */
				mlog_write_ulint(frame +
					FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
					space_id, MLOG_4BYTES, &mtr);

				/* update block */
				block->page.key_version = key_state->key_version;

				/* statistics */
				state->crypt_stat.pages_modified++;
			} else {
				if (crypt_data->is_encrypted()) {
					ut_a(kv >= crypt_data->min_key_version ||
						(kv == 0 && key_state->key_version == 0));

					if (kv < state->min_key_version_found) {
						state->min_key_version_found = kv;
					}
				}
			}

			needs_scrubbing = btr_page_needs_scrubbing(
				&state->scrub_data, block,
				BTR_SCRUB_PAGE_ALLOCATION_UNKNOWN);
		}

		mtr_commit(&mtr);
		lsn_t end_lsn = mtr.end_lsn;

		if (needs_scrubbing == BTR_SCRUB_PAGE) {
			mtr_start(&mtr);
			/*
			* refetch page and allocation status
			*/
			btr_scrub_page_allocation_status_t allocated;
			block = btr_scrub_get_block_and_allocation_status(
				state, space, zip_size, offset, &mtr,
				&allocated,
				&sleeptime_ms);

			if (block) {

				/* get required table/index and index-locks */
				needs_scrubbing = btr_scrub_recheck_page(
					&state->scrub_data, block, allocated, &mtr);

				if (needs_scrubbing == BTR_SCRUB_PAGE) {
					/* we need to refetch it once more now that we have
					* index locked */
					block = btr_scrub_get_block_and_allocation_status(
						state, space, zip_size, offset, &mtr,
						&allocated,
						&sleeptime_ms);

					needs_scrubbing = btr_scrub_page(&state->scrub_data,
						block, allocated,
						&mtr);
				}

				/* NOTE: mtr is committed inside btr_scrub_recheck_page()
				* and/or btr_scrub_page. This is to make sure that
				* locks & pages are latched in corrected order,
				* the mtr is in some circumstances restarted.
				* (mtr_commit() + mtr_start())
				*/
			}
		}

		if (needs_scrubbing != BTR_SCRUB_PAGE) {
			/* if page didn't need scrubbing it might be that cleanups
			are needed. do those outside of any mtr to prevent deadlocks.

			the information what kinds of cleanups that are needed are
			encoded inside the needs_scrubbing, but this is opaque to
			this function (except the value BTR_SCRUB_PAGE) */
			btr_scrub_skip_page(&state->scrub_data, needs_scrubbing);
		}

		if (needs_scrubbing == BTR_SCRUB_TURNED_OFF) {
			/* if we just detected that scrubbing was turned off
			* update global state to reflect this */
			ut_ad(crypt_data);
			mutex_enter(&crypt_data->mutex);
			crypt_data->rotate_state.scrubbing.is_active = false;
			mutex_exit(&crypt_data->mutex);
		}

		if (modified) {
			/* if we modified page, we take lsn from mtr */
			ut_a(end_lsn > state->end_lsn);
			ut_a(end_lsn > block_lsn);
			state->end_lsn = end_lsn;
		} else {
			/* if we did not modify page, check for max lsn */
			if (block_lsn > state->end_lsn) {
				state->end_lsn = block_lsn;
			}
		}
	}

	if (sleeptime_ms) {
		mutex_enter(&fil_crypt_threads_mutex);
		os_event_reset(fil_crypt_throttle_sleep_event);
		mutex_exit(&fil_crypt_threads_mutex);
		os_event_wait_time(fil_crypt_throttle_sleep_event,
				   1000 * sleeptime_ms);
	}
}

/***********************************************************************
Rotate a batch of pages
@param[in,out]		key_state		Key state
@param[in,out]		state			Rotation state */
static
void
fil_crypt_rotate_pages(
	const key_state_t*	key_state,
	rotate_thread_t*	state)
{
	ulint space = state->space->id;
	ulint end = state->offset + state->batch;

	for (; state->offset < end; state->offset++) {

		/* we can't rotate pages in dblwr buffer as
		* it's not possible to read those due to lots of asserts
		* in buffer pool.
		*
		* However since these are only (short-lived) copies of
		* real pages, they will be updated anyway when the
		* real page is updated
		*/
		if (space == TRX_SYS_SPACE &&
		    buf_dblwr_page_inside(state->offset)) {
			continue;
		}

		fil_crypt_rotate_page(key_state, state);
	}
}

/***********************************************************************
Flush rotated pages and then update page 0

@param[in,out]		state	rotation state
@param[in]		space	Tablespace */
static
void
fil_crypt_flush_space(
	rotate_thread_t*	state,
	fil_space_t*		space)
{
	fil_space_crypt_t *crypt_data = space->crypt_data;

	/* flush tablespace pages so that there are no pages left with old key */
	lsn_t end_lsn = crypt_data->rotate_state.end_lsn;

	if (end_lsn > 0 && !space->stop_new_ops) {
		bool success = false;
		ulint n_pages = 0;
		ulint sum_pages = 0;
		ullint start = ut_time_us(NULL);

		do {
			success = buf_flush_list(ULINT_MAX, end_lsn, &n_pages);
			buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
			sum_pages += n_pages;
		} while (!success && !space->stop_new_ops);

		ullint end = ut_time_us(NULL);

		if (sum_pages && end > start) {
			state->cnt_waited += sum_pages;
			state->sum_waited_us += (end - start);

			/* statistics */
			state->crypt_stat.pages_flushed += sum_pages;
		}
	}

	if (crypt_data->min_key_version == 0) {
		crypt_data->type = CRYPT_SCHEME_UNENCRYPTED;
	}

	/* update page 0 */
	mtr_t mtr;
	mtr_start(&mtr);
	ulint offset = 0; // page 0
	const uint zip_size = fsp_flags_get_zip_size(state->space->flags);
	buf_block_t* block = buf_page_get_gen(space->id, zip_size, offset,
				RW_X_LATCH, NULL, BUF_GET,
				__FILE__, __LINE__, &mtr);
	byte* frame = buf_block_get_frame(block);

	crypt_data->page0_offset =
			fsp_header_get_crypt_offset(zip_size);

	fil_space_write_crypt_data(space, frame,
				crypt_data->page0_offset,
				&mtr);
	mtr_commit(&mtr);
}

/***********************************************************************
Complete rotating a space
@param[in,out]		key_state		Key state
@param[in,out]		state			Rotation state */
static
void
fil_crypt_complete_rotate_space(
	const key_state_t*	key_state,
	rotate_thread_t*	state)
{
	fil_space_crypt_t *crypt_data = state->space->crypt_data;

	/* Space might already be dropped */
	if (crypt_data != NULL && !state->space->stop_new_ops) {
		mutex_enter(&crypt_data->mutex);

		/**
		* Update crypt data state with state from thread
		*/
		if (state->min_key_version_found <
			crypt_data->rotate_state.min_key_version_found) {
			crypt_data->rotate_state.min_key_version_found =
				state->min_key_version_found;
		}

		if (state->end_lsn > crypt_data->rotate_state.end_lsn) {
			crypt_data->rotate_state.end_lsn = state->end_lsn;
		}

		ut_a(crypt_data->rotate_state.active_threads > 0);
		crypt_data->rotate_state.active_threads--;
		bool last = crypt_data->rotate_state.active_threads == 0;

		/**
		* check if space is fully done
		* this as when threads shutdown, it could be that we "complete"
		* iterating before we have scanned the full space.
		*/
		bool done = crypt_data->rotate_state.next_offset >=
			crypt_data->rotate_state.max_offset;

		/**
		* we should flush space if we're last thread AND
		* the iteration is done
		*/
		bool should_flush = last && done;

		if (should_flush) {
			/* we're the last active thread */
			crypt_data->rotate_state.flushing = true;
			crypt_data->min_key_version =
				crypt_data->rotate_state.min_key_version_found;
		}

		/* inform scrubbing */
		crypt_data->rotate_state.scrubbing.is_active = false;
		mutex_exit(&crypt_data->mutex);

		/* all threads must call btr_scrub_complete_space wo/ mutex held */
		if (btr_scrub_complete_space(&state->scrub_data) == true) {
			if (should_flush) {
				/* only last thread updates last_scrub_completed */
				ut_ad(crypt_data);
				mutex_enter(&crypt_data->mutex);
				crypt_data->rotate_state.scrubbing.
					last_scrub_completed = time(0);
				mutex_exit(&crypt_data->mutex);
			}
		}

		if (should_flush) {
			fil_crypt_flush_space(state, state->space);

			ut_ad(crypt_data);
			mutex_enter(&crypt_data->mutex);
			crypt_data->rotate_state.flushing = false;
			mutex_exit(&crypt_data->mutex);
		}
	}
}

/*********************************************************************//**
A thread which monitors global key state and rotates tablespaces accordingly
@return a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(fil_crypt_thread)(
/*=============================*/
	void*	arg __attribute__((unused))) /*!< in: a dummy parameter required
					     * by os_thread_create */
{
	UT_NOT_USED(arg);

	mutex_enter(&fil_crypt_threads_mutex);
	uint thread_no = srv_n_fil_crypt_threads_started;
	srv_n_fil_crypt_threads_started++;
	os_event_set(fil_crypt_event); /* signal that we started */
	mutex_exit(&fil_crypt_threads_mutex);

	/* state of this thread */
	rotate_thread_t thr(thread_no);

	/* if we find a space that is starting, skip over it and recheck it later */
	bool recheck = false;

	while (!thr.should_shutdown()) {

		key_state_t new_state;

		time_t wait_start = time(0);

		while (!thr.should_shutdown()) {

			/* wait for key state changes
			* i.e either new key version of change or
			* new rotate_key_age */
			mutex_enter(&fil_crypt_threads_mutex);
			os_event_reset(fil_crypt_threads_event);
			mutex_exit(&fil_crypt_threads_mutex);

			if(srv_fil_crypt_rotate_key_age) {
				if (os_event_wait_time(fil_crypt_threads_event, 1000000) == 0) {
					break;
				}
			} else {
				os_event_wait(fil_crypt_threads_event);
				break;
			}

			if (recheck) {
				/* check recheck here, after sleep, so
				* that we don't busy loop while when one thread is starting
				* a space*/
				break;
			}

			time_t waited = time(0) - wait_start;

			/* Break if we have waited the background scrub
			internal and background scrubbing is enabled */
			if (waited >= srv_background_scrub_data_check_interval
			    && (srv_background_scrub_data_uncompressed
			        || srv_background_scrub_data_compressed)) {
				break;
			}
		}

		recheck = false;
		thr.first = true;      // restart from first tablespace

		/* iterate all spaces searching for those needing rotation */
		while (!thr.should_shutdown() &&
		       fil_crypt_find_space_to_rotate(&new_state, &thr, &recheck)) {

			/* we found a space to rotate */
			fil_crypt_start_rotate_space(&new_state, &thr);

			/* iterate all pages (cooperativly with other threads) */
			while (!thr.should_shutdown() &&
			       fil_crypt_find_page_to_rotate(&new_state, &thr)) {

				/* rotate a (set) of pages */
				fil_crypt_rotate_pages(&new_state, &thr);

				/* Release acquired space after we have
				processet a set of pages to give a chance
				to DROP TABLE or similar.*/
				ulint space_id = thr.space->id;
				fil_space_release(thr.space);
				thr.space = fil_space_acquire_silent(space_id);

				if (!thr.space) {
					break;
				}

				/* realloc iops */
				fil_crypt_realloc_iops(&thr);
			}

			/* complete rotation */
			if (thr.space) {
				fil_crypt_complete_rotate_space(&new_state, &thr);
			}

			/* force key state refresh */
			new_state.key_id= 0;

			/* return iops */
			fil_crypt_return_iops(&thr);
		}
	}

	/* return iops if shutting down */
	fil_crypt_return_iops(&thr);

	/* release current space if shutting down */
	if (thr.space) {
		fil_space_release(thr.space);
		thr.space = NULL;
	}

	mutex_enter(&fil_crypt_threads_mutex);
	srv_n_fil_crypt_threads_started--;
	os_event_set(fil_crypt_event); /* signal that we stopped */
	mutex_exit(&fil_crypt_threads_mutex);

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}

/*********************************************************************
Adjust thread count for key rotation
@param[in]	enw_cnt		Number of threads to be used */
UNIV_INTERN
void
fil_crypt_set_thread_cnt(
	const uint	new_cnt)
{
	if (!fil_crypt_threads_inited) {
		fil_crypt_threads_init();
	}

	if (new_cnt > srv_n_fil_crypt_threads) {
		uint add = new_cnt - srv_n_fil_crypt_threads;
		srv_n_fil_crypt_threads = new_cnt;
		for (uint i = 0; i < add; i++) {
			os_thread_id_t rotation_thread_id;
			os_thread_create(fil_crypt_thread, NULL, &rotation_thread_id);
			ib_logf(IB_LOG_LEVEL_INFO,
				"Creating #%d thread id %lu total threads %u.",
				i+1, os_thread_pf(rotation_thread_id), new_cnt);
		}
	} else if (new_cnt < srv_n_fil_crypt_threads) {
		srv_n_fil_crypt_threads = new_cnt;
		mutex_enter(&fil_crypt_threads_mutex);
		os_event_set(fil_crypt_threads_event);
		mutex_exit(&fil_crypt_threads_mutex);
	}

	while(srv_n_fil_crypt_threads_started != srv_n_fil_crypt_threads) {
		mutex_enter(&fil_crypt_threads_mutex);
		os_event_reset(fil_crypt_event);
		mutex_exit(&fil_crypt_threads_mutex);
		os_event_wait_time(fil_crypt_event, 1000000);
	}
}

/*********************************************************************
Adjust max key age
@param[in]	val		New max key age */
UNIV_INTERN
void
fil_crypt_set_rotate_key_age(
	const uint	val)
{
	srv_fil_crypt_rotate_key_age = val;
	mutex_enter(&fil_crypt_threads_mutex);
	os_event_set(fil_crypt_threads_event);
	mutex_exit(&fil_crypt_threads_mutex);
}

/*********************************************************************
Adjust rotation iops
@param[in]	val		New max roation iops */
UNIV_INTERN
void
fil_crypt_set_rotation_iops(
	const uint val)
{
	srv_n_fil_crypt_iops = val;
	mutex_enter(&fil_crypt_threads_mutex);
	os_event_set(fil_crypt_threads_event);
	mutex_exit(&fil_crypt_threads_mutex);
}

/*********************************************************************
Adjust encrypt tables
@param[in]	val		New setting for innodb-encrypt-tables */
UNIV_INTERN
void
fil_crypt_set_encrypt_tables(
	uint val)
{
	srv_encrypt_tables = val;
	mutex_enter(&fil_crypt_threads_mutex);
	os_event_set(fil_crypt_threads_event);
	mutex_exit(&fil_crypt_threads_mutex);
}

/*********************************************************************
Init threads for key rotation */
UNIV_INTERN
void
fil_crypt_threads_init()
{
	ut_ad(mutex_own(&fil_system->mutex));
	if (!fil_crypt_threads_inited) {
		fil_crypt_event = os_event_create();
		fil_crypt_threads_event = os_event_create();
		mutex_create(fil_crypt_threads_mutex_key,
			&fil_crypt_threads_mutex, SYNC_NO_ORDER_CHECK);

		uint cnt = srv_n_fil_crypt_threads;
		srv_n_fil_crypt_threads = 0;
		fil_crypt_threads_inited = true;
		fil_crypt_set_thread_cnt(cnt);
	}
}

/*********************************************************************
Clean up key rotation threads resources */
UNIV_INTERN
void
fil_crypt_threads_cleanup()
{
	if (!fil_crypt_threads_inited) {
		return;
	}
	ut_a(!srv_n_fil_crypt_threads_started);
	os_event_free(fil_crypt_event);
	os_event_free(fil_crypt_threads_event);
	mutex_free(&fil_crypt_threads_mutex);
	fil_crypt_threads_inited = false;
}

/*********************************************************************
Wait for crypt threads to stop accessing space
@param[in]	space		Tablespace */
UNIV_INTERN
void
fil_space_crypt_close_tablespace(
	const fil_space_t*	space)
{
	if (!srv_encrypt_tables || !space->crypt_data) {
		return;
	}

	mutex_enter(&fil_crypt_threads_mutex);

	fil_space_crypt_t* crypt_data = space->crypt_data;

	time_t start = time(0);
	time_t last = start;

	mutex_enter(&crypt_data->mutex);
	mutex_exit(&fil_crypt_threads_mutex);

	uint cnt = crypt_data->rotate_state.active_threads;
	bool flushing = crypt_data->rotate_state.flushing;

	while (cnt > 0 || flushing) {
		mutex_exit(&crypt_data->mutex);
		/* release dict mutex so that scrub threads can release their
		* table references */
		dict_mutex_exit_for_mysql();
		/* wakeup throttle (all) sleepers */
		mutex_enter(&fil_crypt_threads_mutex);
		os_event_set(fil_crypt_throttle_sleep_event);
		mutex_exit(&fil_crypt_threads_mutex);
		os_thread_sleep(20000);
		dict_mutex_enter_for_mysql();
		mutex_enter(&crypt_data->mutex);
		cnt = crypt_data->rotate_state.active_threads;
		flushing = crypt_data->rotate_state.flushing;

		time_t now = time(0);

		if (now >= last + 30) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Waited %ld seconds to drop space: %s(" ULINTPF ").",
				now - start, space->name, space->id);
			last = now;
		}
	}

	mutex_exit(&crypt_data->mutex);
}

/*********************************************************************
Get crypt status for a space (used by information_schema)
@param[in]	id		Tablespace id
@param[out]	status		Crypt status
return 0 if crypt data present */
UNIV_INTERN
int
fil_space_crypt_get_status(
	const ulint				id,
	struct fil_space_crypt_status_t*	status)
{
	fil_space_t* space = fil_space_acquire(id);
	memset(status, 0, sizeof(*status));

	if (!space) {
		return 0;
	}

	fil_space_crypt_t* crypt_data = space->crypt_data;

	if (crypt_data != NULL) {
		status->space = id;
		status->scheme = crypt_data->type;
		mutex_enter(&crypt_data->mutex);
		status->keyserver_requests = crypt_data->keyserver_requests;
		status->min_key_version = crypt_data->min_key_version;
		status->key_id = crypt_data->key_id;

		if (crypt_data->rotate_state.active_threads > 0 ||
		    crypt_data->rotate_state.flushing) {
			status->rotating = true;
			status->flushing =
				crypt_data->rotate_state.flushing;
			status->rotate_next_page_number =
				crypt_data->rotate_state.next_offset;
			status->rotate_max_page_number =
				crypt_data->rotate_state.max_offset;
		} else {
			status->rotating = false;
		}

		mutex_exit(&crypt_data->mutex);

		if (srv_encrypt_tables || crypt_data->min_key_version) {
			status->current_key_version =
				fil_crypt_get_latest_key_version(crypt_data);
		} else {
			status->current_key_version = 0;
		}
	} else {
		if (srv_encrypt_tables) {
			mutex_enter(&fil_crypt_threads_mutex);
			os_event_set(fil_crypt_threads_event);
			mutex_exit(&fil_crypt_threads_mutex);
		}
	}

	fil_space_release(space);
	return crypt_data == NULL ? 1 : 0;
}

/*********************************************************************
Return crypt statistics
@param[out]	stat		Crypt statistics */
UNIV_INTERN
void
fil_crypt_total_stat(
	fil_crypt_stat_t *stat)
{
	mutex_enter(&crypt_stat_mutex);
	*stat = crypt_stat;
	mutex_exit(&crypt_stat_mutex);
}

/*********************************************************************
Get scrub status for a space (used by information_schema)

@param[in]	id		Space id
@param[out]	status		Scrub status
return 0 if data found */
UNIV_INTERN
int
fil_space_get_scrub_status(
	const ulint				id,
	struct fil_space_scrub_status_t*	status)
{
	fil_space_t* space = fil_space_acquire(id);
	memset(status, 0, sizeof(*status));

	if (!space) {
		return 0;
	}

	fil_space_crypt_t* crypt_data = space->crypt_data;

	memset(status, 0, sizeof(*status));

	if (crypt_data != NULL) {
		status->space = id;
		status->compressed = fil_space_get_zip_size(id) > 0;
		mutex_enter(&crypt_data->mutex);
		status->last_scrub_completed =
			crypt_data->rotate_state.scrubbing.last_scrub_completed;
		if (crypt_data->rotate_state.active_threads > 0 &&
		    crypt_data->rotate_state.scrubbing.is_active) {
			status->scrubbing = true;
			status->current_scrub_started =
				crypt_data->rotate_state.start_time;
			status->current_scrub_active_threads =
				crypt_data->rotate_state.active_threads;
			status->current_scrub_page_number =
				crypt_data->rotate_state.next_offset;
			status->current_scrub_max_page_number =
				crypt_data->rotate_state.max_offset;
		} else {
			status->scrubbing = false;
		}

		mutex_exit(&crypt_data->mutex);
	}

	fil_space_release(space);

	return crypt_data == NULL ? 1 : 0;
}

/******************************************************************
Get crypt data for a tablespace
@param[in]	space		Tablespace */
UNIV_INTERN
fil_space_crypt_t*
fil_space_get_crypt_data(
	fil_space_t*	space)
{
	fil_space_crypt_t* crypt_data = space->crypt_data;

	/* If we have not yet read the page0
	of this tablespace we will do it now. */
	if (!crypt_data && !space->page_0_crypt_read) {
		ulint space_id = space->id;
		fil_node_t*	node;

		ut_a(space->crypt_data == NULL);
		node = UT_LIST_GET_FIRST(space->chain);

		byte *buf = static_cast<byte*>(ut_malloc(2 * UNIV_PAGE_SIZE));
		byte *page = static_cast<byte*>(ut_align(buf, UNIV_PAGE_SIZE));
		fil_read(true, space_id, 0, 0, 0, UNIV_PAGE_SIZE, page,
				NULL, NULL);
		ulint offset = fsp_header_get_crypt_offset(
				fsp_header_get_zip_size(page));
		space->crypt_data = fil_space_read_crypt_data(space_id, page, offset);
		ut_free(buf);

#ifdef UNIV_DEBUG
		ib_logf(IB_LOG_LEVEL_INFO,
			"Read page 0 from tablespace for space %lu name %s key_id %u encryption %d handle %d.",
			space_id,
			space->name,
			space->crypt_data ? space->crypt_data->key_id : 0,
			space->crypt_data ? space->crypt_data->encryption : 0,
			node->handle);
#endif

		ut_a(space->id == space_id);

		space->page_0_crypt_read = true;
		crypt_data = space->crypt_data;
	}

	if (!space->page_0_crypt_read) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Space %lu name %s contains encryption %d information for key_id %u but page0 is not read.",
			space->id,
			space->name,
			space->crypt_data ? space->crypt_data->encryption : 0,
			space->crypt_data ? space->crypt_data->key_id : 0);
	}

	return(crypt_data);
}
