/*
 * Copyright 2019 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "os_file.h"

#include <errno.h>
#include <stdlib.h>

#if defined(__linux__)

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>


static ssize_t
readN(int fd, char *buf, size_t len)
{
   int err = -ENODATA;
   size_t total = 0;
   do {
      ssize_t ret = read(fd, buf + total, len - total);

      if (ret < 0)
         ret = -errno;

      if (ret == -EINTR || ret == -EAGAIN)
         continue;

      if (ret <= 0)
         break;

      total += ret;
   } while (total != len);

   return total ? total : err;
}

static char *
read_grow(int fd)
{
   size_t len = 64;

   char *buf = malloc(len);
   if (!buf) {
      close(fd);
      errno = -ENOMEM;
      return NULL;
   }

   ssize_t read;
   size_t offset = 0, remaining = len - 1;
   while ((read = readN(fd, buf + offset, remaining)) == remaining) {
      char *newbuf = realloc(buf, 2 * len);
      if (!newbuf) {
         free(buf);
         close(fd);
         errno = -ENOMEM;
         return NULL;
      }

      buf = newbuf;
      len *= 2;
      offset += read;
      remaining = len - offset - 1;
   }

   close(fd);

   if (read > 0)
      offset += read;

   buf[offset] = '\0';

   return buf;
}

char *
os_read_file(const char *filename)
{
   size_t len = 0;

   int fd = open(filename, O_RDONLY);
   if (fd == -1) {
      /* errno set by open() */
      return NULL;
   }

   struct stat stat;
   if (fstat(fd, &stat) == 0)
      len = stat.st_size;

   if (!len)
      return read_grow(fd);

   /* add NULL terminator */
   len++;

   char *buf = malloc(len);
   if (!buf) {
      close(fd);
      errno = -ENOMEM;
      return NULL;
   }

   ssize_t read = readN(fd, buf, len - 1);

   close(fd);

   if (read == -1)
      return NULL;

   buf[read] = '\0';

   return buf;
}

#else

char *
os_read_file(const char *filename)
{
   errno = -ENOSYS;
   return NULL;
}

#endif
