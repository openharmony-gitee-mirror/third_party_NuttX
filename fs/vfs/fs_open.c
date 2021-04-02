/****************************************************************************
 * fs/vfs/fs_open.c
 *
 *   Copyright (C) 2007-2009, 2011-2012, 2016-2018 Gregory Nutt. All rights
 *     reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "vfs_config.h"

#include "errno.h"
#include "sys/types.h"
#include "fcntl.h"
#include "sched.h"
#include "assert.h"
#ifdef LOSCFG_FILE_MODE
#include "stdarg.h"
#endif
#include "stdlib.h"
#include "fs/fs.h"
#include "fs/vnode.h"
#include "driver/blockproxy.h"
#include "fs_other.h"
#include "fs/vfs_util.h"
#include "fs/path_cache.h"
#include "unistd.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static int oflag_convert_mode(int oflags)
{
  /* regular file operations */

  int acc_mode = 0;
  if ((oflags & O_ACCMODE) == O_RDONLY)
  acc_mode |= READ_OP;
  if (oflags & O_WRONLY)
  acc_mode |= WRITE_OP;
  if (oflags & O_RDWR)
  acc_mode |= READ_OP | WRITE_OP;

  /* Opens the file, if it is existing. If not, a new file is created. */

  if (oflags & O_CREAT)
  acc_mode |= WRITE_OP;

  /* Creates a new file. If the file is existing, it is truncated and overwritten. */

  if (oflags & O_TRUNC)
  acc_mode |= WRITE_OP;

  /* Creates a new file. The function fails if the file is already existing. */

  if (oflags & O_EXCL)
  acc_mode |= WRITE_OP;
  if (oflags & O_APPEND)
  acc_mode |= WRITE_OP;

  /* mark for executing operation */

  if (oflags & O_EXECVE)
  acc_mode |= EXEC_OP;
  return acc_mode;
}

int get_path_from_fd(int fd, char **path)
{
  struct file *file     = NULL;
  char            *copypath = NULL;

  if (fd == AT_FDCWD)
    {
      return OK;
    }

  int ret = fs_getfilep(fd, &file);
  if (ret < 0)
    {
      return -ENOENT;
    }

  if ((file == NULL) || (file->f_vnode == NULL) || (file->f_path == NULL))
    {
      return -EBADF;
    }

  copypath = strdup((const char*)file->f_path);
  if (copypath == NULL)
    {
      return VFS_ERROR;
    }

  char *endptr = copypath + strlen(copypath)-1;//the ptr before '\0'

  /* strip out the file name, for example:/usr/lib/xx.so, final get /usr/lib/ */
  while (endptr > copypath)
    {
      if(*endptr == '/' && endptr > copypath)
        {
          *(endptr + 1) = '\0';
          *path = copypath;
          return OK;
        }

      endptr--;
    }

  free(copypath);
  return -ENOENT;
}

static int do_creat(struct Vnode **node, char *fullpath, mode_t mode)
{
  int ret;
  struct Vnode *parentNode = *node;
  char *name = strrchr(fullpath, '/') + 1;

  if (parentNode->vop != NULL && parentNode->vop->Create != NULL)
    {
      ret = parentNode->vop->Create(parentNode, name, mode, node);
    }
  else
    {
      ret = -ENOSYS;
    }

  if (ret < 0)
    {
      return ret;
    }

  struct PathCache *dt = PathCacheAlloc(parentNode, *node, name, strlen(name));
  if (dt == NULL)
    {
      // alloc name cache failed is not a critical problem, let it go.
      PRINT_ERR("alloc path cache %s failed\n", name);
    }
  return OK;
}

int fp_open(char *fullpath, int oflags, mode_t mode)
{
  int ret;
  int fd;
  int accmode;
  struct file *filep = NULL;
  struct Vnode *vnode = NULL;

  VnodeHold();
  ret = VnodeLookup(fullpath, &vnode, 0);
  if (ret == OK)
    {
      /* if file exist */
      if (vnode->type == VNODE_TYPE_BCHR)
        {
          ret = -EINVAL;
          goto errout_without_count;
        }
#ifdef LOSCFG_FS_VFS_BLOCK_DEVICE
      if (vnode->type == VNODE_TYPE_BLK) {
          VnodeDrop();
          fd = block_proxy(fullpath, oflags);
          if (fd < 0)
            {
              ret = fd;
              goto errout_without_count;
            }
         return fd;
      }
#endif
      if ((oflags & O_CREAT) && (oflags & O_EXCL))
        {
          ret = -EEXIST;
          VnodeDrop();
          goto errout_without_count;
        }
      if (vnode->type == VNODE_TYPE_DIR)
        {
          ret = -EISDIR;
          VnodeDrop();
          goto errout_without_count;
        }
      accmode = oflag_convert_mode(oflags);
      if (VfsVnodePermissionCheck(vnode, accmode))
        {
          ret = -EACCES;
          VnodeDrop();
          goto errout_without_count;
        }
    }

  if ((ret != OK) && (oflags & O_CREAT) && vnode)
    {
      /* if file not exist, but parent dir of the file is exist */
      if (VfsVnodePermissionCheck(vnode, WRITE_OP))
        {
          ret = -EACCES;
          VnodeDrop();
          goto errout_without_count;
        }
      ret = do_creat(&vnode, fullpath, mode);
      if (ret != OK)
        {
          VnodeDrop();
          goto errout_without_count;
        }
    }

  if (ret != OK)
    {
      /* found nothing */
      VnodeDrop();
      goto errout_without_count;
    }
  vnode->useCount++;
  VnodeDrop();

  if (oflags & O_TRUNC)
    {
      if (vnode->useCount > 1)
        {
          ret = -EBUSY;
          goto errout;
        }

      if (vnode->vop->Truncate)
        {
          ret = vnode->vop->Truncate(vnode, 0);
          if (ret != OK)
            {
              goto errout;
            }
        }
      else
        {
          ret = -ENOSYS;
          goto errout;
        }
    }

  fd = files_allocate(vnode, oflags, 0, NULL, 3); /* 3: file start fd */
  if (fd < 0)
    {
      ret = -EMFILE;
      goto errout;
    }

  /* Get the file structure corresponding to the file descriptor. */
  ret = fs_getfilep(fd, &filep);
  if (ret < 0)
    {
      files_release(fd);
      ret = -get_errno();
      goto errout;
    }

  filep->f_vnode = vnode;
  filep->ops = vnode->fop;
  filep->f_path = fullpath;

  if (filep->ops && filep->ops->open)
    {
      ret = filep->ops->open(filep);
    }

  if (ret < 0)
    {
      files_release(fd);
      goto errout;
    }

  /* we do not bother to handle the NULL scenario, if so, page-cache feature will not be used
   * when we do the file fault */
#ifdef LOSCFG_KERNEL_VM
  add_mapping(filep, fullpath);
#endif

  return fd;

errout:
  VnodeHold();
  vnode->useCount--;
  VnodeDrop();
errout_without_count:
  set_errno(-ret);
  return VFS_ERROR;
}

int do_open(int dirfd, const char *path, int oflags, mode_t mode)
{
  int ret;
  int fd;
  char *fullpath          = NULL;
  char *relativepath     = NULL;

  /* Get relative path by dirfd*/
  ret = get_path_from_fd(dirfd, &relativepath);
  if (ret < 0)
    {
      goto errout;
    }

  ret = vfs_normalize_path((const char *)relativepath, path, &fullpath);
  if (relativepath)
    {
      free(relativepath);
    }
  if (ret < 0)
    {
      goto errout;
    }

  fd = fp_open(fullpath, oflags, mode);
  if (fd < 0)
    {
      ret = -get_errno();
      goto errout;
    }

  return fd;

errout:
  if (fullpath)
    {
      free(fullpath);
    }
  set_errno(-ret);
  return VFS_ERROR;
}

/****************************************************************************
 * Name: open
 *
 * Description: Standard 'open' interface
 *
 ****************************************************************************/

int open(const char *path, int oflags, ...)
{
  mode_t mode = DEFAULT_FILE_MODE; /* File read-write properties. */
#ifdef LOSCFG_FILE_MODE
  va_list ap;
  va_start(ap, oflags);
  mode = va_arg(ap, int);
  va_end(ap);

  if ((oflags & (O_WRONLY | O_CREAT)) != 0)
    {
      mode &= ~GetUmask();
      mode &= (S_IRWXU|S_IRWXG|S_IRWXO);
    }
#endif

  return do_open(AT_FDCWD, path, oflags, mode);
}

int open64 (const char *__path, int __oflag, ...)
{
  mode_t mode = DEFAULT_FILE_MODE; /* File read-write properties. */
#ifdef LOSCFG_FILE_MODE
  va_list ap;
  va_start(ap, __oflag);
  mode = va_arg(ap, int);
  va_end(ap);
  if ((__oflag & (O_WRONLY | O_CREAT)) != 0)
    {
      mode &= ~GetUmask();
      mode &= (S_IRWXU|S_IRWXG|S_IRWXO);
    }
#endif
  return open (__path, ((unsigned int)__oflag) | O_LARGEFILE, mode);
}
