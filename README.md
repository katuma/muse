What is it?
-----------
MUSE is a simple implementation of multiple mounts merging filesystem
(something like unionfs, but without support of read-only volumes) - No COW,
moving between underlying partitions or whiteouting is implemented.

Why...
------
.. aren't there plenty of unioning filesystems already?
Well, perhaps. The closest thing to this is mhddfs, which is getting
really slow on bigger disk clusters. MUSE is a reimplementation of idea while
trying perform ok even with really big filesystems. (25 volumes, tens of
terabytes..).



How to use it
-------------
Compile it, you may have to fiddle with the makefile, especially on systems
other than Linux. Then:

    ./muse -o allow_other,default_permissions dirlist.txt /mnt/muse

dirlist.txt is just plain text with line-separated list of directories, for example:

    /mnt/sda1
    /mnt/sdb1
    /mnt/sdc1
    ...

How is it decided where is what
-------------------------------
When a new file or directory is created, it is placed to the first volume where
parent directory is found, for example if following directories are in place:

/dev/sda1/a
/dev/sdb1/b
/dev/sdc1/b

(ie just /a and /b merged)

And we want to create /b/foo, it will be placed in /dev/sdb1/b. However, if
there will be less free space than m_minfree (see source) we'll look for other
alternatives meeting the limit, at worst doing equivalent of mkdir -p where
needed.

What's broken?
--------------
- The whole thing is not really well tested. DO NOT USE FOR IMPORTANT DATA.
  DATA LOSS IS GUARANTEED.
- rename() is somewhat racy, it might just die in the middle.
- If you'll fix something, please send a pull request.
