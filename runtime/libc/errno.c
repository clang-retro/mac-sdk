/* File Manager and Memory Manager results as errno values.
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#include <errno.h>
#include "macos.h"

int __mac_errno(OSErr err) {
  switch (err) {
  case fnfErr: case nsvErr: case dirNFErr: return ENOENT;
  case dupFNErr: return EEXIST;
  case bdNamErr: return EINVAL;
  case tmfoErr: return EMFILE;
  case dskFulErr: case dirFulErr: return ENOSPC;
  case wPrErr: case vLckdErr: case fLckdErr: case permErr: case wrPermErr:
  case -5000: /* afpAccessDenied, on a file server */
    return EACCES;
  case opWrErr: case fBsyErr: return EBUSY;
  case posErr: case paramErr: return EINVAL;
  case rfNumErr: case fnOpnErr: return EBADF;
  case memFullErr: return ENOMEM;
  default: return EIO;
  }
}
