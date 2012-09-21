What is it?
-----------
MUSE is a simple implementation of multiple mounts merging filesystem
(something like unionfs, but without support of read-only volumes).

Why...
------
.. aren't there plenty of unioning filesystems already?

Well, perhaps. The closest thing to this is mhddfs, which is getting
really slow on bigger disk clusters. MUSE is a reimplementation of
idea while trying perform ok even with really big filesystems.
(25 volumes, hundreds of terabytes..).

MUSE is not really a COW filesystem, but something more akin to raid0
array.

There is no COW, moving across underlying partitions or whiteouting
is implemented. Because there is no need to - it's just writing to
all underlying filesystems.

It will always try to pick single fs for directory subtree of data,
until it runs out of free disk space, then it moves to another disk
unioned.

It also plays some dirty tricks to maintain reasonable performance.
In particular, regular files are just symlinks to actual under-fs
storage. Hence FUSE is avoided for read/writes altogether. However,
rename() will move the actual file under the symlink, unlink()
will delete the real file as well.



How to use it
-------------
Compile it, you may have to fiddle with the makefile, especially on
systems other than Linux. Then:

    ./muse dirlist.txt /mnt/muse -o allow_other,default_permissions

dirlist.txt is just plain text with line-separated list of directories,
for example:

    /mnt/sda1
    /mnt/sdb1
    /mnt/sdc1
    ...

How is it decided where is what
-------------------------------
When a new file or directory is created, it is placed to the first
volume where parent directory is found, for example if following
directories are in place:

/dev/sda1/a
/dev/sdb1/b
/dev/sdc1/b

(ie just /a and /b merged)

And we want to create /b/foo, it will be placed in /dev/sdb1/b.
However, if there will be less free space than MINFREE (8GB)
we'll look for other alternatives meeting the limit, at worst
doing equivalent of mkdir -p where needed.

What's broken?
--------------
- The whole thing is not really well tested.
  DO NOT USE FOR IMPORTANT DATA. DATA LOSS IS GUARANTEED.
- rename() is somewhat racy, it might just die in the middle.
- If you'll fix something, please send a pull request.
