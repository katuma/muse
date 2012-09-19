/* vim: set noexpandtab:
 * MUSE filesystem
 *
 * Copyright (c) 2005-2012 Karel Tuma, karel.tuma@gmail.com.
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* free space threshold for full search (8GB default) */
#define MINFREE (8*1024*1024*1024LL)

/* use symlinks */
#define SYMLINK_HACK 1

#define FUSE_USE_VERSION 26
#define _XOPEN_SOURCE 500
#define _BSD_SOURCE 1
#include <fuse.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <utime.h>
#include <sys/types.h>
#include <alloca.h>
#include <signal.h>

/* dupe entries in a directory */
struct	dupeent {
	struct dupeent *next;
	char name[1];
};

/* freshly created files we should *never* symlink to */
struct created_file {
	struct created_file *next;
	int count;
	char path[1];
};

struct created_file *created_files;

#define MAXDIRS 64
#define DUPES_MASK 16383

#if DEBUG
#define D(a...) { int es=errno; fprintf(stderr, "@@@@ " a); fprintf(stderr, "\n"); errno=es; }
#else
#define D(x...) {}
#endif

/********************************************
 * globals
 ********************************************/

static	unsigned long m_bsize=4096;
static	unsigned long m_namemax=256;

/* path list */
static	char *rlist[MAXDIRS];
static	uint64_t rfree[MAXDIRS]; /* free bytes on each path */
static	int rcount, rmaxlen;
static 	char *cfg;

/********************************************
 * macros
 ********************************************/
#define PATH_TO_REAL_IDX(path,idx) \
	char rto[rmaxlen+1+strlen(path)]; \
	strcpy(rto, rlist[idx]); \
	strcat(rto, path);

/* execute statement for each real path of given virtual 'path'.
 * variables:
 * 	elm - current real path
 * 	idx - current real path index
 */
#define FOR_EACH_REAL(path) \
	char elm[rmaxlen+1+strlen(path)]; \
	int idx; \
	errno = 0; \
	for (idx = 0; idx < rcount && (strcat(strcpy(elm, rlist[idx]), path)); idx++)


/* apply command 'cmd' for every real path of virtual path 'path',
 * escape on first success or error other than ENOENT
 * variables to be used in cmd:
 * 	elm, idx, res
 */
#define APPLY_FOR_REAL_RES(path, res, cmd) \
	FOR_EACH_REAL(path) { \
		res=cmd; \
		if (res>=0) goto done; \
		if (errno != ENOENT && errno != ENOTDIR) return -(errno); \
	} \
	return -(errno=ENOENT); \
done:;

/* ditto as above but return */
#define APPLY_FOR_REAL(path, cmd) \
	int res=0; \
	APPLY_FOR_REAL_RES(path, res, cmd); \
	return 0;

#define FIND_NEW_REAL(old) \
	char np[rmaxlen+2+strlen(old)]; \
	if (find_new_real(old, np)) return -(errno); \
	errno=0;

#define UPLEVEL(path) \
	strrchr(path, '/')[0]=0;


/* do what mkdir -p does. path is virtual path to be created,
 * srcidx is source volume where 'path' resides,
 * dstidx is where the path is to be created. */
static int mkdirp_path(const char *path, int srcidx, int dstidx)
{
	char srcbuf[strlen(path)+1+rmaxlen];
	char dstbuf[strlen(path)+1+rmaxlen];
	int i = 1;
	char *sd, *dd;
	strcpy(srcbuf, rlist[srcidx]);
	strcpy(dstbuf, rlist[dstidx]);
	sd = srcbuf + strlen(srcbuf);
	dd = dstbuf + strlen(dstbuf);
	*dd++ = '/';
	*sd++ = '/';
	D("entered mkdirp_path(%s,%d,%d)",path,srcidx,dstidx);
	while (path[i]) {
		struct stat st;
		struct utimbuf utb;
		while ((path[i] != '/') && path[i]) { *sd++ = path[i]; *dd++ = path[i]; i++; }
		*sd=*dd=0;
//		D("i=%d sd=%s dd=%s",i,srcbuf,dstbuf);
		if (lstat(srcbuf, &st))
			return -errno;
		if (!(S_IFDIR&st.st_mode))
			return -(errno=ENOTDIR);
		/* XXX TODO - this is racy. we should create temporary name chown/chmod it
		 * and then rename to something visible */
		if (mkdir(dstbuf, st.st_mode & 0777) && errno != EEXIST)
			return -errno;
		utb.actime = st.st_atime;
		utb.modtime = st.st_mtime;
		if (lchown(dstbuf, st.st_uid, st.st_gid) ||
		    utime(dstbuf, &utb))
		    	return -errno;
		if (path[i] == '/') { *sd++ = *dd++ = '/'; i++; };
	}
	return (errno=0);
}

static int find_new_real(const char *old, char *buf)
{
	int bestdir=-1; /* best partition where directory is already in place */
	int okfree=-1; /* best partition above limit */
	int bestfree=0; /* best partition (below limit, but most free space) */
	D("find new %s", old);
	FOR_EACH_REAL(old) {
		UPLEVEL(elm);
		D("check %s\n", elm);
		/* figure out best space */
		if (rfree[idx] > rfree[bestfree]) bestfree=idx;
		/* figure out enough space */
		if (okfree < 0 && rfree[idx] > MINFREE) okfree=idx;
		/* figure out where's the most free space and
		 * directory already in place */
		if (access(elm, F_OK)) continue;
		if (bestdir<0 || rfree[idx] > rfree[bestdir]) bestdir=idx;
	}

	/* no dir there, probably race. */
	if (bestdir < 0) return -(errno=ENOTDIR);

	/* current dir has not much space && there's better alternative */
	if (okfree < 0) okfree=bestfree;
	if (rfree[bestdir] < MINFREE) {
		char tmp[PATH_MAX+1];
		strcpy(tmp, old);
		UPLEVEL(tmp);
		if (mkdirp_path(tmp,bestdir,okfree))
			return -errno;
		bestdir=okfree;
	}

	strcpy(buf, rlist[bestdir]);
	strcat(buf, old);
	errno=0;
	return 0;
}

static void chowner(const char *path, int mode)
{
	struct stat st;
	gid_t gid = fuse_get_context()->gid;
	uid_t uid = fuse_get_context()->uid;
	if (!lstat(path, &st) && st.st_gid!=getgid())
		gid=st.st_gid;
	lchown(path, uid, gid);
	if (mode) chmod(path, mode);
}
/********************************************
 * handlers
 ********************************************/
static int muse_readlink(const char *path, char *buf, size_t size)
{
	int first_errno = 0;
	FOR_EACH_REAL(path) {
		struct stat st;
		if (!lstat(elm, &st)) {
			// XXX todo *real* links
			strcpy(buf, elm);
			return 0;
		} else if (!first_errno)
			first_errno = errno;
	}
	return first_errno;
}

static uint32_t hash(const char *s)
{
	uint32_t h = 5381;
	while (*s)
		h = ((h << 5) + h) + *s++;
	return h;
}

static int muse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi)
{
	DIR *d;
	struct dirent *de;
	int added=0;
	struct dupeent *dp, *dupes[DUPES_MASK+1] = { NULL };
	uint32_t hk;

	errno=0;
	D("readdir for %s", path);
	FOR_EACH_REAL(path) {
		D("processing %s", elm);
//retry:;
		if (!(d = opendir(elm))) {
			D("opendir %s failed: %d", elm, errno);
			/* theres no real dir, dismiss */
			if (errno == ENOENT || errno == ENOTDIR) {
				errno=0;
				continue;
			}
#if 0
			/* other error, might be just permission desync */
			if (check_perm_sync())
				goto retry;
#endif
			/* some other error, stop whereever we are */
			break;
		}
		added++;
#define DHASH dupes[hk & DUPES_MASK]
		while ((de = readdir(d))) {
			hk = hash(de->d_name);
			struct stat st;
			for (dp = DHASH; dp; dp = dp->next) {
				/* dupe found? */
				if (!strcmp(dp->name, de->d_name)) {
					D("dupe found: %s", dp->name);
					break;
				}
			}
			if (dp) continue; /* dupe */

			/* some filesystems dont report .. */
			if (de->d_type == DT_UNKNOWN) {
				char tmp[PATH_MAX+1];
				strcpy(tmp, elm);
				strcat(tmp, "/");
				strcat(tmp, de->d_name);

				/* huh? */
				if (lstat(tmp, &st)) continue;
			} else {
				/* its fine, post it */
				st.st_ino = de->d_ino;
				st.st_mode = de->d_type << 12; // glibc specific, XXX switch statement?
			}

			/* new dupe entry */
			dp = alloca(sizeof(*dp) + strlen(de->d_name));
			strcpy(dp->name, de->d_name);
			dp->next = DHASH;
			DHASH = dp;

			if (filler(buf, de->d_name, &st, 0)) {
				D("filler fail");
				closedir(d);
				goto out;
			}
		}
		closedir(d);
		D("errno %d", errno);
	}
out:;
	if (!added && !errno) errno = ENOENT;
	return -errno;
}

static int muse_mknod(const char *path, mode_t mode, dev_t rdev)
{
	FIND_NEW_REAL(path);
	if (S_ISFIFO(mode))
		mkfifo(np, mode);
	else
		mknod(np, mode, rdev);
	if (!errno)
		chowner(np, 0);
	return -errno;
}

static int muse_mkdir(const char *path, mode_t mode)
{
	FIND_NEW_REAL(path);

	//if (np=find_new_real(path))
	//	return -(errno = EEXIST);

	if (!mkdir(np, 0)) {
		chowner(np, mode);
		return 0;
	}
	return -errno;
}

static int muse_unlink(const char *path)
{
	int res;

	/* always try to delete the file everywhere, essential for fixing desynchs :P */
	FOR_EACH_REAL(path) {
		res = unlink(elm);
		/* ok, something got deleted..*/
#ifdef BE_FAST
		if (!res) break;
#endif
		if (res < 0 && errno != ENOENT && errno != ENOTDIR) return -errno;
	}

	return 0;
}

static int muse_rmdir(const char *path)
{
	int count=0;

	FOR_EACH_REAL(path) {
		if (rmdir(elm) && errno != ENOENT && errno != ENOTDIR) {
			return -errno;
		} else count++;
	}
	if (!count) return -(errno = ENOENT);
	return (errno=0);
}

static int muse_symlink(const char *from, const char *to)
{
	FIND_NEW_REAL(to);

	if (!symlink(from, np)) chowner(np, 0);
	return -errno;
}

/*
 * this is possibly broken
 */
static int muse_rename(const char *from, const char *to)
{
	int first_errno=0;
	int done = 0;

	/* for each source path */
	FOR_EACH_REAL(from) {
		PATH_TO_REAL_IDX(to, idx);
		if (rename(elm, rto)) {
			int i;
			struct stat st;
			char tmp[PATH_MAX+1];
			/* source object not present, just skip it. */
			if (lstat(elm, &st)) {
				if (errno == ENOENT) continue;
				if (!first_errno) first_errno = errno;
			}

			/* try to create full path at destination */
			strcpy(tmp, to);
			UPLEVEL(tmp);
			for (i = 0; i < rcount; i++)
				if (!mkdirp_path(tmp, i, idx)) break;

			/* something failed along the way? */
			if (i == rcount) continue;

			/* ok, retry the rename. it should succeed this time. */
			if (!rename(elm, rto)) {
				done++;
			} else {
				if (!first_errno) first_errno = errno;
			}
		} else done++;
	}
	if (done) return 0;
	return first_errno==0?-EIO:-first_errno;
}


static int muse_link(const char *from, const char *to)
{
	FOR_EACH_REAL(from) {
		PATH_TO_REAL_IDX(to,idx);
		if (link(elm, rto)) {
			if (errno == ENOENT || errno == ENOTDIR) continue;
			return -(errno);
		}
		return 0;
	}

	return -errno;
}

static int muse_chmod(const char *path, mode_t mode)
{
	APPLY_FOR_REAL(path, chmod(elm, mode));
}

static int muse_chown(const char *path, uid_t uid, gid_t gid)
{
	APPLY_FOR_REAL(path, lchown(elm, uid, gid));
}

static int muse_truncate(const char *path, off_t size)
{
	APPLY_FOR_REAL(path, truncate(elm, size));
}

static int muse_utimens(const char *path, const struct timespec ts[2])
{
	struct timeval tv[2];

	tv[0].tv_sec = ts[0].tv_sec;
	tv[0].tv_usec = ts[0].tv_nsec / 1000;
	tv[1].tv_sec = ts[1].tv_sec;
	tv[1].tv_usec = ts[1].tv_nsec / 1000;

	APPLY_FOR_REAL(path, utimes(elm, tv));
}


static int muse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	int res;
	struct created_file *ce;
	FIND_NEW_REAL(path);
	res=open(np, fi->flags, mode);
	if (res<0) return -errno;
	chowner(np, 0);

#if SYMLINK_HACK
	ce = malloc(sizeof(*ce) + strlen(path));
	ce->next = created_files;
	created_files = ce;
	ce->count = 1;
	strcpy(ce->path, path);
#endif
	fi->fh=res;
	return 0;
}

/* Fuse kernel part is retarded. Whenever it detects that opened
   file has changed under its hands, it'll stubbornly return -EIO.
   Hence, we'll track newly created files and won't dare to symlink
   em until kernel releases all the references. Sheesh.
 */
static struct created_file *find_cf(const char *path, int inc)
{
	struct created_file *ce, *prev;
	for (ce = created_files, prev = NULL; ce; ce = ce->next) {
		if (!strcmp(path, ce->path)) {
			ce->count += inc;
			if (ce->count > 0) return ce;
			if (!prev)
				created_files = ce->next;
			else
				prev->next = ce->next;
			free(ce);
			return NULL;
		}
		prev = ce;
	}
	return NULL;
}

static int muse_open(const char *path, struct fuse_file_info *fi)
{
	int res;
	APPLY_FOR_REAL_RES(path, res, open(elm, fi->flags));
	fi->fh = res;
	find_cf(path, 1);
	return 0;
}

static int muse_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	int res = pread(fi->fh, buf, size, offset);
	if (res<0) return -errno;
	return res;
}

static int muse_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	int res = pwrite(fi->fh, buf, size, offset);
	if (res<0) return -errno;
	return res;
}

static int muse_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	int res;
	if (isdatasync)
		res=fdatasync(fi->fh);
	else
		res=fsync(fi->fh);
	if (res<0) return -(errno);
	return 0;
}

static int muse_statfs(const char *path, struct statvfs *stb)
{
	struct statvfs st;

	memset(stb, 0, sizeof(*stb));
	FOR_EACH_REAL(path) {
		if (statvfs(elm, &st)) continue;
		stb->f_blocks+=(st.f_blocks*st.f_bsize);
		stb->f_bfree+=(rfree[idx]=(st.f_bfree*st.f_bsize));
		stb->f_bavail+=(st.f_bavail*st.f_bsize);
		stb->f_files+=st.f_files;
		stb->f_ffree+=st.f_ffree;
		stb->f_favail+=st.f_favail;
	}
	errno=0;
	stb->f_bsize=m_bsize;
	stb->f_frsize=m_bsize;
	stb->f_blocks/=m_bsize;
	stb->f_bfree/=m_bsize;
	stb->f_bavail/=m_bsize;
	stb->f_namemax=m_namemax;
	return 0;
}

static int muse_release(const char *path, struct fuse_file_info *fi)
{
	find_cf(path, -1);
	if (close(fi->fh))
		return -(errno);
	return 0;
}

static int muse_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	APPLY_FOR_REAL(path, lsetxattr(elm, name, value, size, flags));
}

static int muse_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	APPLY_FOR_REAL(path, lgetxattr(elm, name, value, size));
}

static int muse_listxattr(const char *path, char *list, size_t size)
{
	APPLY_FOR_REAL(path, llistxattr(elm, list, size));
}

static int muse_removexattr(const char *path, const char *name)
{
	APPLY_FOR_REAL(path, lremovexattr(elm, name));
}

static int muse_getattr(const char *path, struct stat *st)
{
	int first_errno = 0;
	FOR_EACH_REAL(path) {
		if (lstat(elm, st)) {
			if (errno == ENOENT)
				continue;
			if (!first_errno)
				first_errno = errno;
			continue;
		}
#if SYMLINK_HACK
		/* this will hopefully force kernel to route us through readlink() */
		if (S_ISREG(st->st_mode) && !find_cf(path, 0)) {
			st->st_mode &= S_IFMT;
			st->st_mode |= S_IFLNK;
		}
#endif
		return 0;
	}
	return first_errno==0?-ENOENT:-first_errno;
}



static int muse_access(const char *path, int mask)
{
	APPLY_FOR_REAL(path, access(elm, mask));
}


static struct fuse_operations muse_oper = {
	.getattr	= muse_getattr,
	.access		= muse_access,
	.readlink	= muse_readlink,
	.readdir	= muse_readdir,
	.mknod		= muse_mknod,
	.mkdir		= muse_mkdir,
	.symlink	= muse_symlink,
	.unlink		= muse_unlink,
	.rmdir		= muse_rmdir,
	.rename		= muse_rename,
	.link		= muse_link,
	.chmod		= muse_chmod,
	.chown		= muse_chown,
	.truncate	= muse_truncate,
	.utimens	= muse_utimens,
	.open		= muse_open,
	.create		= muse_create,
	.read		= muse_read,
	.write		= muse_write,
	.fsync		= muse_fsync,
	.statfs		= muse_statfs,
	.release	= muse_release,
	.setxattr	= muse_setxattr,
	.getxattr	= muse_getxattr,
	.listxattr	= muse_listxattr,
	.removexattr	= muse_removexattr,
};

void reread_config(int d)
{
	char ln[PATH_MAX+1];
	char *rlist_new[MAXDIRS];
	int rc = 0;
	FILE *f = fopen(cfg, "rt");
	if (!f && d==-1) { perror(cfg); exit(2); };
	while (fgets(ln, sizeof(ln)-1, f)) {
		char *p = strchr(ln, '\n');
		if (p) *p = 0;
		if (d==-1 && chdir(ln)) { perror(ln); exit(3); };
		if (d==-1 && rc>=MAXDIRS) { fprintf(stderr, "Too many directories (limit %d)\n", MAXDIRS); exit(4); };
		rlist_new[rc++] = strdup(ln);
		if (strlen(ln) > rmaxlen) rmaxlen = strlen(ln);
	}
	fclose(f);
	rcount = rc;
	memcpy(rlist, rlist_new, sizeof(rlist));
}


int main(int argc, char *argv[])
{

	umask(0);
	printf("MUSE filesystem 1.1\n2008-2012 karel.tuma@gmail.com\n\n");
	if (argc < 3) {
		fprintf(stderr, "Usage:\n%s dirlist.txt /mountpoint -o allow_other,default_permissions \n\n"
			"- dirlist is a newline separated list of directories to use\n"
			"- send SIGHUP to the running processs to reread the list\n\n",
			argv[0]);
		return 1;
	}
	cfg = argv[1];
	reread_config(-1);
	signal(SIGHUP, reread_config);
	return fuse_main(argc-1, argv+1, &muse_oper, NULL);
}

