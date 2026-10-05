/*
 * auth.c
 *
 * Copyright (c) 2026 Eric Vidal <eric@obarun.org>
 *
 * All rights reserved.
 *
 * This file is part of Obarun. It is subject to the license terms in
 * the LICENSE file found in the top-level directory of this
 * distribution.
 * This file may not be copied, modified, propagated, or distributed
 * except according to the terms contained in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h> // rename
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <oblibs/directory.h>
#include <oblibs/fd.h>
#include <oblibs/files.h>
#include <oblibs/io.h>
#include <oblibs/log.h>
#include <oblibs/string.h>
#include <oblibs/types.h>

#include <66/constants.h>

#include "auth.h"

#define AUTH_PASS_MAX 1024
#define URANDOM "/dev/urandom"
#define AUTH_FILE_MAX 512
#define AUTH_KDF_ROUNDS 100000
#define SHA256_BLOCK 64
#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_BSIG0(x) (SHA256_ROTR(x, 2) ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_BSIG1(x) (SHA256_ROTR(x, 6) ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_SSIG0(x) (SHA256_ROTR(x, 7) ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_SSIG1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

typedef struct sha256_s sha256_t ;
struct sha256_s {
	uint32_t state[8] ;
	uint64_t bitlen ;
	unsigned char block[SHA256_BLOCK] ;
	size_t blocklen ;
} ;

static uint32_t const SHA256_K[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
	0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
	0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
	0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
	0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
	0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
	0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
} ;

static void sha256_compress(uint32_t *state, unsigned char const *block)
{
	uint32_t w[64] ;
	uint32_t a, b, c, d, e, f, g, h ;
	size_t i = 0 ;

	for (; i < 16 ; i++) {
		w[i] = ((uint32_t)block[i * 4]     << 24) |
			   ((uint32_t)block[i * 4 + 1] << 16) |
			   ((uint32_t)block[i * 4 + 2] <<  8) |
			   (uint32_t)block[i * 4 + 3] ;
	}

	for (i = 16 ; i < 64 ; i++)
		w[i] = SHA256_SSIG1(w[i - 2]) + w[i - 7] + SHA256_SSIG0(w[i - 15]) + w[i - 16] ;

	a = state[0] ; b = state[1] ; c = state[2] ; d = state[3] ;
	e = state[4] ; f = state[5] ; g = state[6] ; h = state[7] ;

	for (size_t i = 0 ; i < 64 ; i++) {
		uint32_t t1 = h + SHA256_BSIG1(e) + SHA256_CH(e, f, g) + SHA256_K[i] + w[i] ;
		uint32_t t2 = SHA256_BSIG0(a) + SHA256_MAJ(a, b, c) ;
		h = g ; g = f ; f = e ; e = d + t1 ;
		d = c ; c = b ; b = a ; a = t1 + t2 ;
	}

	state[0] += a ; state[1] += b ; state[2] += c ; state[3] += d ;
	state[4] += e ; state[5] += f ; state[6] += g ; state[7] += h ;
}

static void sha256_begin(sha256_t *ctx)
{
	static uint32_t const IV[8] = {
		0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
		0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
	} ;

	memcpy(ctx->state, IV, sizeof(IV)) ;
	ctx->bitlen = 0 ;
	ctx->blocklen = 0 ;
}

static void sha256_absorb(sha256_t *ctx, void const *data, size_t len)
{
	unsigned char const *p = (unsigned char const *)data ;

	ctx->bitlen += (uint64_t)len * 8 ;

	while (len) {

		size_t room = SHA256_BLOCK - ctx->blocklen ;
		size_t take = len < room ? len : room ;

		memcpy(ctx->block + ctx->blocklen, p, take) ;
		ctx->blocklen += take ;
		p += take ;
		len -= take ;

		if (ctx->blocklen == SHA256_BLOCK) {
			sha256_compress(ctx->state, ctx->block) ;
			ctx->blocklen = 0 ;
		}
	}
}

static void sha256_end(sha256_t *ctx, unsigned char *digest)
{
	uint64_t bitlen = ctx->bitlen ;

	ctx->block[ctx->blocklen++] = 0x80 ;

	if (ctx->blocklen > SHA256_BLOCK - 8) {
		memset(ctx->block + ctx->blocklen, 0, SHA256_BLOCK - ctx->blocklen) ;
		sha256_compress(ctx->state, ctx->block) ;
		ctx->blocklen = 0 ;
	}

	memset(ctx->block + ctx->blocklen, 0, SHA256_BLOCK - 8 - ctx->blocklen) ;

	for (size_t i = 0 ; i < 8 ; i++)
		ctx->block[SHA256_BLOCK - 1 - i] = (unsigned char)(bitlen >> (i * 8)) ;

	sha256_compress(ctx->state, ctx->block) ;

	for (size_t i = 0 ; i < 8 ; i++) {
		digest[i * 4] = (unsigned char)(ctx->state[i] >> 24) ;
		digest[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16) ;
		digest[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8) ;
		digest[i * 4 + 3] = (unsigned char) ctx->state[i] ;
	}
}

static void wipe(void *p, size_t len)
{
	volatile unsigned char *v = (volatile unsigned char *)p ;
	while (len--)
		*v++ = 0 ;
}

static void derive(unsigned char const *salt, char const *pass, unsigned char *out)
{
	sha256_t ctx ;
	unsigned char h[AUTH_HASH_LEN] ;

	sha256_begin(&ctx) ;
	sha256_absorb(&ctx, salt, AUTH_SALT_LEN) ;
	sha256_absorb(&ctx, pass, strlen(pass)) ;
	sha256_end(&ctx, h) ;

	for (unsigned int i = 1 ; i < AUTH_KDF_ROUNDS ; i++) {
		sha256_begin(&ctx) ;
		sha256_absorb(&ctx, salt, AUTH_SALT_LEN) ;
		sha256_absorb(&ctx, h, AUTH_HASH_LEN) ;
		sha256_end(&ctx, h) ;
	}

	memcpy(out, h, AUTH_HASH_LEN) ;
	wipe(&ctx, sizeof(ctx)) ;
	wipe(h, sizeof(h)) ;
}

static int ct_memeq(void const *a, void const *b, size_t len)
{
	unsigned char const *x = (unsigned char const *)a ;
	unsigned char const *y = (unsigned char const *)b ;
	unsigned char diff = 0 ;

	for (size_t i = 0 ; i < len ; i++)
		diff |= (unsigned char)(x[i] ^ y[i]) ;

	return !diff ;
}

static int ct_streq(char const *a, char const *b)
{
	char pa[AUTH_USER_MAX] = { 0 } ;
	char pb[AUTH_USER_MAX] = { 0 } ;
	size_t la = strlen(a) ;
	size_t lb = strlen(b) ;

	if (la >= AUTH_USER_MAX || lb >= AUTH_USER_MAX)
		return 0 ;

	memcpy(pa, a, la) ;
	memcpy(pb, b, lb) ;

	return ct_memeq(pa, pb, AUTH_USER_MAX) ;
}

static int hex_decode(char const *in, size_t outlen, unsigned char *out)
{
	unsigned int byte = 0 ;

	for (size_t i = 0 ; i < outlen ; i++) {

		byte = 0 ;
		for (size_t k = 0 ; k < 2 ; k++) {
			char ch = in[i * 2 + k] ;
			unsigned char v = scan_digit((unsigned char)ch, 16) ;
			if (v >= 16)
				return 0 ;
			byte = (byte << 4) | v ;
		}
		out[i] = (unsigned char)byte ;
	}
	return 1 ;
}

static void hex_encode(unsigned char const *in, size_t inlen, char *out)
{
	for (size_t i = 0 ; i < inlen ; i++) {
		out[i * 2] = fmt_digit((unsigned char)(in[i] >> 4)) ;
		out[i * 2 + 1] = fmt_digit((unsigned char)(in[i] & 0x0f)) ;
	}
	out[inlen * 2] = 0 ;
}

static void prompt(char const *s)
{
	size_t len = strlen(s) ;
	io_allwrite(2, (char *)s, len) ;
}

static int read_line(int fd, char *store, size_t cap, size_t *len)
{
	size_t n = 0 ;

	for (;;) {
		char ch ;
		ssize_t r = io_read(fd, &ch, 1) ;
		if (r < 0)
			return 0 ;
		if (!r) {
			if (!n)
				return (errno = EPIPE, 0) ;
			break ;
		}
		if (ch == '\n') break ;
		if (n + 1 >= cap)
			return (errno = ENAMETOOLONG, 0) ;
		store[n++] = ch ;
	}

	store[n] = 0 ;
	*len = n ;
	return 1 ;
}

static int ensure_parent_dir(char const *path)
{
	char dir[SS_MAX_PATH_LEN + 1] ;
	char const *slash = strrchr(path, '/') ;
	size_t len ;
	struct stat st ;

	if (!slash || slash == path)
		return 1 ;

	len = (size_t)(slash - path) ;
	if (len >= SS_MAX_PATH_LEN + 1)
		return (errno = ENAMETOOLONG, 0) ;

	memcpy(dir, path, len) ;
	dir[len] = 0 ;

	if (!stat(dir, &st))
		return S_ISDIR(st.st_mode) ? 1 : (errno = ENOTDIR, 0) ;

	if (errno != ENOENT)
		return 0 ;

	return dir_create_parent(dir, 0700) ;
}

static int write_atomic_0600(char const *path, char const *line, size_t linelen)
{
	char tmp[SS_MAX_PATH_LEN + 1] ;
	char pidbuf[PID_FMT] ;
	int fd ;

	if (!ensure_parent_dir(path))
		return 0 ;

	pidbuf[pid_format(pidbuf, getpid())] = 0 ;

	if (strlen(path) + 5 + strlen(pidbuf) >= SS_MAX_PATH_LEN + 1)
		return (errno = ENAMETOOLONG, 0) ;

	auto_strings(tmp, path, ".tmp.", pidbuf) ;

	fd = io_open_mode(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600) ;
	if (fd < 0) return 0 ;

	/* The creation mode is filtered by the umask; force the bits back. */
	if (fchmod(fd, 0600) < 0 ||
		io_allwrite(fd, (char *)line, linelen) != linelen ||
		fsync(fd) < 0) {
			close_fd(fd) ;
			file_tryunlink(tmp) ;
			return 0 ;
	}

	if (close(fd) < 0) {
		file_tryunlink(tmp) ;
		return 0 ;
	}

	if (rename(tmp, path) < 0) {
		file_tryunlink(tmp) ;
		return 0 ;
	}

	return 1 ;
}

static int write_credential(char const *path, char const *user, char const *pass)
{
	unsigned char salt[AUTH_SALT_LEN] ;
	unsigned char hash[AUTH_HASH_LEN] ;
	char salthex[AUTH_SALT_LEN * 2 + 1] ;
	char hashhex[AUTH_HASH_LEN * 2 + 1] ;
	char line[AUTH_FILE_MAX] ;
	int ok ;

	if (file_read(URANDOM, (char *)salt, AUTH_SALT_LEN) != (ssize_t)AUTH_SALT_LEN)
		log_warnusys_return(0, "read the random salt from " URANDOM) ;

	derive(salt, pass, hash) ;
	hex_encode(salt, AUTH_SALT_LEN, salthex) ;
	hex_encode(hash, AUTH_HASH_LEN, hashhex) ;

	/* Bounded by construction: `user` is at most AUTH_USER_MAX - 1 = 255
	 * bytes, so the line is at most 354 bytes and always fits AUTH_FILE_MAX. */
	auto_strings(line, user, ":", salthex, ":", hashhex, "\n") ;

	ok = write_atomic_0600(path, line, strlen(line)) ;
	if (!ok)
		log_warnusys("write credential file: ", path) ;

	wipe(hash, sizeof(hash)) ;
	wipe(hashhex, sizeof(hashhex)) ;
	wipe(line, sizeof(line)) ;

	return ok ;
}

int auth_load(char const *path, auth_cred_t *cred)
{
	char buf[AUTH_FILE_MAX] ;
	unsigned char salt[AUTH_SALT_LEN] ;
	unsigned char hash[AUTH_HASH_LEN] ;
	char const *colon ;
	size_t ulen ;
	ssize_t n ;

	if (!path || !cred) return (errno = EINVAL, 0) ;

	n = file_read(path, buf, AUTH_FILE_MAX - 1) ;
	if (n < 0)
		return 0 ;

	if ((size_t)n >= AUTH_FILE_MAX - 1)
		return 0 ;

	if (memchr(buf, 0, (size_t)n))
		return 0 ; // embedded NUL: not a text line
	buf[n] = 0 ;

	if (n && buf[n - 1] == '\n')
		buf[--n] = 0 ;

	if (memchr(buf, '\n', (size_t)n))
		return 0 ; // more than one line

	colon = (char const *)memchr(buf, ':', (size_t)n) ;
	if (!colon)
		return 0 ;

	ulen = (size_t)(colon - buf) ;

	if (!ulen || ulen >= AUTH_USER_MAX)
		return 0 ;

	// user ':' salt_hex ':' hash_hex, and nothing else.
	if ((size_t)n != ulen + 1 + AUTH_SALT_LEN * 2 + 1 + AUTH_HASH_LEN * 2)
		return 0 ;

	if (colon[1 + AUTH_SALT_LEN * 2] != ':')
		return 0 ;

	if (!hex_decode(colon + 1, AUTH_SALT_LEN, salt))
		return 0 ;

	if (!hex_decode(colon + 1 + AUTH_SALT_LEN * 2 + 1, AUTH_HASH_LEN, hash))
		return 0 ;

	memcpy(cred->user, buf, ulen) ;
	cred->user[ulen] = 0 ;

	memcpy(cred->salt, salt, AUTH_SALT_LEN) ;
	memcpy(cred->hash, hash, AUTH_HASH_LEN) ;

	return 1 ;
}

int auth_check(auth_cred_t const *cred, char const *user, char const *pass)
{
	unsigned char hash[AUTH_HASH_LEN] ;
	int ok ;

	if (!cred || !user || !pass)
		return (errno = EINVAL, 0) ;

	/* Both halves are always evaluated: & instead of &&, so a wrong user
	 * costs exactly as much as a wrong password. */
	derive(cred->salt, pass, hash) ;
	ok = ct_streq(cred->user, user) & ct_memeq(hash, cred->hash, AUTH_HASH_LEN) ;

	wipe(hash, sizeof(hash)) ;

	return ok ;
}

int auth_passwd_interactive(char const *path)
{
	char user[AUTH_USER_MAX] ;
	char pass[AUTH_PASS_MAX] ;
	char confirm[AUTH_PASS_MAX] ;
	struct termios saved ;
	size_t ulen = 0, plen = 0, clen = 0 ;
	int echo_off = 0 ;
	int ret = 0 ;

	if (!path)
		return (errno = EINVAL, 0) ;

	prompt("Username: ") ;
	if (!read_line(0, user, AUTH_USER_MAX, &ulen))
		log_warnusys_return(0, "read user name") ;

	if (!ulen)
		log_warn_return(0, "empty user name") ;

	if (memchr(user, ':', ulen))
		log_warn_return(0, "user name must not contain ':'") ;

	if (isatty(0)) {

		struct termios noecho ;
		if (tcgetattr(0, &saved) < 0)
			log_warnusys_return(0, "read terminal attributes") ;

		noecho = saved ;
		noecho.c_lflag &= ~(tcflag_t)ECHO ;

		if (tcsetattr(0, TCSAFLUSH, &noecho) < 0)
			log_warnusys_return(0, "disable terminal echo") ;

		echo_off = 1 ;
	}

	prompt("Password: ") ;
	if (!read_line(0, pass, AUTH_PASS_MAX, &plen)) {
		log_warnusys("read password") ;
		goto out ;
	}

	if (echo_off)
		prompt("\n") ;

	prompt("Confirm password: ") ;
	if (!read_line(0, confirm, AUTH_PASS_MAX, &clen)) {
		log_warnusys("read password confirmation") ;
		goto out ;
	}

	if (echo_off)
		prompt("\n") ;

	if (!plen) {
		log_warn("empty password") ;
		goto out ;
	}

	if (plen != clen || memcmp(pass, confirm, plen)) {
		log_warn("passwords do not match") ;
		goto out ;
	}

	ret = write_credential(path, user, pass) ;
	if (ret)
		log_info("credential written to ", path) ;

out:
	if (echo_off && tcsetattr(0, TCSAFLUSH, &saved) < 0)
		log_warnusys("restore terminal echo") ;
	wipe(pass, sizeof(pass)) ;
	wipe(confirm, sizeof(confirm)) ;

	return ret ;
}

int host_is_loopback(char const *host)
{
	struct in_addr v4 ;
	struct in6_addr v6 ;
	char unbracketed[64] ;
	size_t len ;

	if (!host)
		return 0 ;

	if (!strcmp(host, "localhost"))
		return 1 ;

	if (inet_pton(AF_INET, host, &v4) == 1)
		return (ntohl(v4.s_addr) >> 24) == 127 ;

	len = strlen(host) ;
	if (len >= 2 && host[0] == '[' && host[len - 1] == ']') {

		if (len - 2 >= sizeof(unbracketed))
			return 0 ;
		memcpy(unbracketed, host + 1, len - 2) ;
		unbracketed[len - 2] = 0 ;

		host = unbracketed ;
	}

	if (inet_pton(AF_INET6, host, &v6) == 1)
		return IN6_IS_ADDR_LOOPBACK(&v6) ;

	return 0 ;
}
