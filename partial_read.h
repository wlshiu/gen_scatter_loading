/**
 * Copyright (c) 2016 Wei-Lun Hsu. All Rights Reserved.
 */
/** @file partial_read.h
 *
 * @author Wei-Lun Hsu
 * @version 0.1
 * @date 2016/12/01
 * @license
 * @description
 */

#ifndef __partial_read_H_wEvw3Yzn_l2k4_HBBC_sXGl_uAFLkimY3eSv__
#define __partial_read_H_wEvw3Yzn_l2k4_HBBC_sXGl_uAFLkimY3eSv__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <string.h>
//=============================================================================
//                  Constant Definition
//=============================================================================

//=============================================================================
//                  Macro Definition
//=============================================================================

//=============================================================================
//                  Structure Definition
//=============================================================================
typedef struct partial_read
{
    FILE            *fp;
    long            file_size;
    long            file_remain;

    unsigned char   *pBuf;
    long            buf_size;
    long            buf_remain_data;

    unsigned char   *pCur;
    unsigned char   *pEnd;

    int             alignment;
    unsigned long   is_big_endian;
    unsigned long   is_restart;


} partial_read_t;
//=============================================================================
//                  Global Data Definition
//=============================================================================

//=============================================================================
//                  Private Function Definition
//=============================================================================

//=============================================================================
//                  Public Function Definition
//=============================================================================
static inline int
partial_read__full_buf(
    partial_read_t  *pHReader,
    int (*cb_post_read)(unsigned char *pBuf, int buf_size))
{
    int         rval = 0;

    do {
        size_t      nbytes = 0;
        long        remain_data = 0;

        if( pHReader->is_restart )
        {
            fseek(pHReader->fp, 0l, SEEK_SET);
            pHReader->file_remain = pHReader->file_size;

            pHReader->pCur = pHReader->pBuf;
            pHReader->pEnd = pHReader->pBuf;

            pHReader->is_restart = 0;
        }

        remain_data = pHReader->pEnd - pHReader->pCur;
        if( pHReader->file_remain &&
            remain_data < (pHReader->buf_size >> 2) )
        {
            if( remain_data )
                memmove(pHReader->pBuf, pHReader->pCur, remain_data);

            // full buffer
            nbytes = fread(pHReader->pBuf + remain_data, 1, pHReader->buf_size - remain_data, pHReader->fp);

            pHReader->pCur = pHReader->pBuf;
            pHReader->pEnd = pHReader->pBuf + remain_data + nbytes;

            pHReader->file_remain -= nbytes;

            // after reading process
            if( cb_post_read &&
                (rval = cb_post_read(pHReader->pBuf + remain_data, nbytes)) )
               break;
        }

        pHReader->buf_remain_data = pHReader->pEnd - pHReader->pCur;

    } while(0);

    return rval;
}


#ifdef __cplusplus
}
#endif

#endif
