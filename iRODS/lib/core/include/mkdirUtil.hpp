/*** Copyright (c), The Regents of the University of California            ***
 *** For more information please refer to files in the COPYRIGHT directory ***/
/* getUtil.h - Header for for getUtil.c */

#ifndef MKDIR_UTIL_HPP
#define MKDIR_UTIL_HPP

#include "rodsClient.hpp"
#include "parseCommandLine.hpp"
#include "rodsPath.hpp"

#ifdef __cplusplus
extern "C" {
#endif
int
mkdirUtil( rcComm_t *conn, rodsArguments_t *myRodsArgs,
           rodsPathInp_t *rodsPathInp );

#ifdef __cplusplus
}
#endif

#endif	/* MKDIR_UTIL_H */
