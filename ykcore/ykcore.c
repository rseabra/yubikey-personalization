/* -*- mode:C; c-file-style: "bsd" -*- */
/*
 * Copyright (c) 2008, 2009, Yubico AB
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ykcore_lcl.h"
#include "ykcore_backend.h"
#include "yktsd.h"

/* To get modhex and crc16 */
#include <yubikey.h>

#ifndef _WIN32
#include <unistd.h>
#define Sleep(x) usleep((x)*1000)
#endif

int yk_init(void)
{
	return _ykusb_start();
}

int yk_release(void)
{
	return _ykusb_stop();
}

YK_KEY *yk_open_first_key(void)
{
	YK_KEY *yk = _ykusb_open_device(YUBICO_VID, YUBIKEY_PID);
	int rc = yk_errno;

	if (yk) {
		YK_STATUS st;

		if (!yk_get_status(yk, &st)) {
			rc = yk_errno;
			yk_close_key(yk);
			yk = NULL;
		} else {
			if (!((st.versionMajor == 1 &&
			       (st.versionMinor == 0 ||
				st.versionMinor == 1 ||
				st.versionMinor == 2 ||
				st.versionMinor == 3)) ||
			      (st.versionMajor == 2 &&
			       st.versionMinor == 0))) {
				rc = YK_EFIRMWARE;
				yk_close_key(yk);
				yk = NULL;
			}
		}
	}
	yk_errno = rc;
	return yk;
}

int yk_close_key(YK_KEY *yk)
{
	return _ykusb_close_device(yk);
}

int yk_get_status(YK_KEY *k, YK_STATUS *status)
{
	unsigned int status_count = 0;

	if (!yk_read_from_key(k, 0, status, sizeof(YK_STATUS), &status_count))
		return 0;

	if (status_count != sizeof(YK_STATUS)) {
		yk_errno = YK_EWRONGSIZ;
		return 0;
	}

	status->touchLevel = yk_endian_swap_16(status->touchLevel);

	return 1;
}

int yk_write_config(YK_KEY *yk, YK_CONFIG *cfg, int confnum,
		    unsigned char *acc_code)
{
	unsigned char buf[sizeof(YK_CONFIG) + ACC_CODE_SIZE];
	YK_STATUS stat;
	int seq;

	/* Get current seqence # from status block */

	if (!yk_get_status(yk, &stat /*, 0*/))
		return 0;

	seq = stat.pgmSeq;

	/* Update checksum and insert config block in buffer if present */

	memset(buf, 0, sizeof(buf));

	if (cfg) {
		cfg->crc = ~yubikey_crc16 ((unsigned char *) cfg,
					   sizeof(YK_CONFIG) - sizeof(cfg->crc));
		cfg->crc = yk_endian_swap_16(cfg->crc);
		memcpy(buf, cfg, sizeof(YK_CONFIG));
	}

	/* Append current access code if present */

	if (acc_code)
		memcpy(buf + sizeof(YK_CONFIG), acc_code, ACC_CODE_SIZE);

	/* Write to Yubikey */

	switch(confnum) {
	case 1:
		if (!yk_write_to_key(yk, SLOT_CONFIG, buf, sizeof(buf)))
			return 0;
		break;
	case 2:
		if (!yk_write_to_key(yk, SLOT_CONFIG2, buf, sizeof(buf)))
			return 0;
		break;
	}

	/* Verify update */

	if (!yk_get_status(yk, &stat /*, 0*/))
		return 0;

	yk_errno = YK_EWRITEERR;
	if (cfg) {
		return stat.pgmSeq != seq;
	}

	return stat.pgmSeq == 0;

}

int * const _yk_errno_location(void)
{
	static int tsd_init = 0;
	static int nothread_errno = 0;
	YK_DEFINE_TSD_METADATA(errno_key);
	int rc = 0;

	if (tsd_init == 0) {
		if ((rc = YK_TSD_INIT(errno_key, free)) == 0) {
			YK_TSD_SET(errno_key, calloc(1, sizeof(int)));
			tsd_init = 1;
		} else {
			tsd_init = -1;
		}
	}
	if (tsd_init == 1) {
		return YK_TSD_GET(int *, errno_key);
	}
	return &nothread_errno;
}

static const char *errtext[] = {
	"",
	"USB error",
	"wrong size",
	"write error",
	"timeout",
	"no yubikey present",
	"unsupported firmware version",
	"out of memory",
	"no status structure given",
	"not yet implemented"
};
const char *yk_strerror(int errnum)
{
	if (errnum < sizeof(errtext)/sizeof(errtext[0]))
		return errtext[errnum];
	return NULL;
}
const char *yk_usb_strerror()
{
	return _ykusb_strerror();
}

/* Note: we currently have no idea whatsoever how to read things larger
   than FEATURE_RPT_SIZE - 1.  We also have no idea what to do with the
   slot parameter, it currently is there for future purposes only. */
int yk_read_from_key(YK_KEY *yk, uint8_t slot,
		     void *buf, unsigned int bufsize, unsigned int *bufcount)
{
	unsigned char data[FEATURE_RPT_SIZE];

	if (bufsize > FEATURE_RPT_SIZE - 1) {
		yk_errno = YK_EWRONGSIZ;
		return 0;
	}

	memset(data, 0, sizeof(data));

	if (!_ykusb_read(yk, REPORT_TYPE_FEATURE, 0, (char *)data, FEATURE_RPT_SIZE))
		return 0;

	/* This makes it apparent that there's some mysterious value in
	   the first byte...  I wonder what...  /Richard Levitte */
	memcpy(buf, data + 1, bufsize); 
	*bufcount = bufsize;

	return 1;
}

int yk_write_to_key(YK_KEY *yk, uint8_t slot, const void *buf, int bufcount)
{
	unsigned char repbuf[FEATURE_RPT_SIZE];
	unsigned char data[SLOT_DATA_SIZE + FEATURE_RPT_SIZE];
	int i, j, pos, part;

	/* Insert data and set slot # */

	memset(data, 0, sizeof(data));
	memcpy(data, buf, bufcount);
	data[SLOT_DATA_SIZE] = slot;

	/* Append slot checksum */

	i = yubikey_crc16 (data, SLOT_DATA_SIZE);
	data[SLOT_DATA_SIZE + 1] = (unsigned char) (i & 0xff);
	data[SLOT_DATA_SIZE + 2] = (unsigned char) (i >> 8);

	/* Chop up the data into parts that fits into the payload of a
	   feature report. Set the part number | 0x80 in the end
	   of the feature report. When the Yubikey has processed it,
	   it will clear this byte, signaling that the next part can be
	   sent */

	for (pos = 0, part = 0x80; pos < (SLOT_DATA_SIZE + 4); part++) {

		/* Ignore parts that are all zeroes except first and last
		   to speed up the transfer */

		for (i = j = 0; i < (FEATURE_RPT_SIZE - 1); i++)
			if ((repbuf[i] = data[pos++])) j = 1;
		if (!j && (part > 0x80) && (pos < SLOT_DATA_SIZE))
			continue;

		repbuf[i] = part;

		if (!_ykusb_write(yk, REPORT_TYPE_FEATURE, 0,
				  (char *)repbuf, FEATURE_RPT_SIZE))
			return 0;

		/* When the last byte in the feature report is cleared by
		   the Yubikey, the next part can be sent */

		for (i = 0; i < 50; i++) {
			memset(repbuf, 0, sizeof(repbuf));
			if (!_ykusb_read(yk, REPORT_TYPE_FEATURE, 0,
					 (char *)repbuf, FEATURE_RPT_SIZE))
				return 0;
			if (!repbuf[FEATURE_RPT_SIZE - 1])
				break;
			Sleep(10);
		}

		/* If timeout, something has gone wrong */

		if (i >= 50) {
			yk_errno = YK_ETIMEOUT;
			return 0;
		}
	}

	return 1;
}

int yk_force_key_update(YK_KEY *yk)
{
	unsigned char buf[FEATURE_RPT_SIZE];

	memset(buf, 0, sizeof(buf));
	buf[FEATURE_RPT_SIZE - 1] = 0x8a; /* Invalid partition = update only */
	if (!_ykusb_write(yk, REPORT_TYPE_FEATURE, 0, (char *)buf, FEATURE_RPT_SIZE))
		return 0;

	return 1;
}

uint16_t yk_endian_swap_16(uint16_t x)
{
	static int testflag = -1;

	if (testflag == -1) {
		uint16_t testword = 0x0102;
		unsigned char *testchars = (unsigned char *)&testword;
		if (*testchars == '\1')
			testflag = 1; /* Big endian arch, swap needed */
		else
			testflag = 0; /* Little endian arch, no swap needed */
	}

	if (testflag)
		x = (x >> 8) | ((x & 0xff) << 8);

	return x;
}

