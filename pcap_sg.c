/*
 * netsniff-ng - the packet sniffing beast
 * By Daniel Borkmann <daniel@netsniff-ng.org>
 * Copyright 2011 - 2013 Daniel Borkmann.
 * Subject to the GPL, version 2.
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include "pcap.h"
#include "xmalloc.h"
#include "xio.h"
#include "xutils.h"
#include "built_in.h"

static struct iovec iov[1024] __cacheline_aligned;
static off_t iov_off_rd = 0, iov_slot = 0;

static ssize_t pcap_sg_write(int fd, pcap_pkthdr_t *phdr, enum pcap_type type,
			     const uint8_t *packet, size_t len)
{
	ssize_t ret;

	if (unlikely(iov_slot == array_size(iov))) {
		ret = writev(fd, iov, array_size(iov));
		if (ret < 0)
			panic("Writev I/O error: %s!\n", strerror(errno));

		iov_slot = 0;
	}

	iov[iov_slot].iov_len = 0;
	switch (type) {
#define PCAP_HDR_WRITE(__member__) do { \
			fmemcpy(iov[iov_slot].iov_base, &phdr->__member__, \
				sizeof(phdr->__member__)); \
			iov[iov_slot].iov_len += sizeof(phdr->__member__); \
		} while(0)
#define CASE_HDR_WRITE(what, member) \
	case (what): \
		PCAP_HDR_WRITE(member); \
		break
	CASE_HDR_WRITE(DEFAULT, ppo);
	CASE_HDR_WRITE(NSEC, ppn);
	CASE_HDR_WRITE(KUZNETZOV, ppk);
	CASE_HDR_WRITE(BORKMANN, ppb);
	default:
		bug();
	}

	fmemcpy(iov[iov_slot].iov_base + iov[iov_slot].iov_len, packet, len);
	ret = (iov[iov_slot].iov_len += len);
	iov_slot++;

	return ret;
}

static ssize_t __pcap_sg_inter_iov_hdr_read(int fd, pcap_pkthdr_t *phdr, enum pcap_type type,
					    uint8_t *packet, size_t len, size_t hdrsize)
{
	int ret;
	size_t offset = 0;
	ssize_t remainder;

	offset = iov[iov_slot].iov_len - iov_off_rd;
	remainder = hdrsize - offset;
	if (remainder < 0)
		remainder = 0;

	bug_on(offset + remainder != hdrsize);

	switch (type) {
#define CASE_HDR_PREAD(what, __member__) \
		case (what): \
			fmemcpy(&phdr->__member__, \
				iov[iov_slot].iov_base + iov_off_rd, offset); \
			break
	CASE_HDR_PREAD(DEFAULT, ppo);
	CASE_HDR_PREAD(NSEC, ppn);
	CASE_HDR_PREAD(KUZNETZOV, ppk);
	CASE_HDR_PREAD(BORKMANN, ppb);
	default:
		bug();
	}

	iov_off_rd = 0;
	iov_slot++;

	if (iov_slot == array_size(iov)) {
		iov_slot = 0;
		ret = readv(fd, iov, array_size(iov));
		if (unlikely(ret <= 0))
			return -EIO;
	}

	switch (type) {
#define CASE_HDR_RREAD(what, __member__) \
		case (what): \
			fmemcpy(&phdr->__member__ + offset, \
				iov[iov_slot].iov_base + iov_off_rd, remainder); \
			break
	CASE_HDR_RREAD(DEFAULT, ppo);
	CASE_HDR_RREAD(NSEC, ppn);
	CASE_HDR_RREAD(KUZNETZOV, ppk);
	CASE_HDR_RREAD(BORKMANN, ppb);
	default:
		bug();
	}

	iov_off_rd += remainder;

	return hdrsize;
}

static ssize_t __pcap_sg_inter_iov_data_read(int fd, uint8_t *packet, size_t len, size_t hdrlen)
{
	int ret;
	size_t offset = 0;
	ssize_t remainder;

	offset = iov[iov_slot].iov_len - iov_off_rd;
	remainder = hdrlen - offset;
	if (remainder < 0)
		remainder = 0;

	bug_on(offset + remainder != hdrlen);

	fmemcpy(packet, iov[iov_slot].iov_base + iov_off_rd, offset);
	iov_off_rd = 0;
	iov_slot++;

	if (iov_slot == array_size(iov)) {
		iov_slot = 0;
		ret = readv(fd, iov, array_size(iov));
		if (unlikely(ret <= 0))
			return -EIO;
	}

	fmemcpy(packet + offset, iov[iov_slot].iov_base + iov_off_rd, remainder);
	iov_off_rd += remainder;

	return hdrlen;
}

static ssize_t pcap_sg_read(int fd, pcap_pkthdr_t *phdr, enum pcap_type type,
			    uint8_t *packet, size_t len)
{
	ssize_t ret = 0;
	size_t hdrsize = pcap_get_hdr_length(phdr, type), hdrlen;

	if (likely(iov[iov_slot].iov_len - iov_off_rd >= hdrsize)) {
		switch (type) {
#define CASE_HDR_READ(what, __member__) \
		case (what): \
			fmemcpy(&phdr->__member__, \
				iov[iov_slot].iov_base + iov_off_rd, hdrsize); \
			break
		CASE_HDR_READ(DEFAULT, ppo);
		CASE_HDR_READ(NSEC, ppn);
		CASE_HDR_READ(KUZNETZOV, ppk);
		CASE_HDR_READ(BORKMANN, ppb);
		default:
			bug();
		}

		iov_off_rd += hdrsize;
	} else {
		ret = __pcap_sg_inter_iov_hdr_read(fd, phdr, type, packet,
						   len, hdrsize);
		if (unlikely(ret < 0))
			return ret;
	}

	hdrlen = pcap_get_length(phdr, type);
	if (unlikely(hdrlen == 0 || hdrlen > len))
		return -EINVAL;

	if (likely(iov[iov_slot].iov_len - iov_off_rd >= hdrlen)) {
		fmemcpy(packet, iov[iov_slot].iov_base + iov_off_rd, hdrlen);
		iov_off_rd += hdrlen;
	} else {
		ret = __pcap_sg_inter_iov_data_read(fd, packet, len, hdrlen);
		if (unlikely(ret < 0))
			return ret;
	}

	return hdrsize + hdrlen;
}

static void pcap_sg_fsync(int fd)
{
	ssize_t ret = writev(fd, iov, iov_slot);
	if (ret < 0)
		panic("Writev I/O error: %s!\n", strerror(errno));

	iov_slot = 0;
	fdatasync(fd);
}

static int pcap_sg_prepare_access(int fd, enum pcap_mode mode, bool jumbo)
{
	int i, ret;
	size_t len = 0;

	iov_slot = 0;
	len = jumbo ? (PAGE_SIZE * 16) /* 64k max */ :
		      (PAGE_SIZE *  3) /* 12k max */;

	for (i = 0; i < array_size(iov); ++i) {
		iov[i].iov_base = xzmalloc_aligned(len, 64);
		iov[i].iov_len = len;
	}

	set_ioprio_rt();

	if (mode == PCAP_MODE_RD) {
		ret = readv(fd, iov, array_size(iov));
		if (ret <= 0)
			return -EIO;

		iov_off_rd = 0;
		iov_slot = 0;
	}

	return 0;
}

static void pcap_sg_prepare_close(int fd, enum pcap_mode mode)
{
	int i;

	for (i = 0; i < array_size(iov); ++i)
		xfree(iov[i].iov_base);
}

const struct pcap_file_ops pcap_sg_ops = {
	.pull_fhdr_pcap = pcap_generic_pull_fhdr,
	.push_fhdr_pcap = pcap_generic_push_fhdr,
	.prepare_access_pcap =  pcap_sg_prepare_access,
	.prepare_close_pcap = pcap_sg_prepare_close,
	.read_pcap = pcap_sg_read,
	.write_pcap = pcap_sg_write,
	.fsync_pcap = pcap_sg_fsync,
};
